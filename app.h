#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl3w.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>

#include "shader.h"
#include "repaging.h"
#include "loader_thread.h"

void message_callback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, GLchar const* message, void const* user_param)
{
	auto const src_str = [source]() {
		switch (source)
		{
		case GL_DEBUG_SOURCE_API: return "API";
		case GL_DEBUG_SOURCE_WINDOW_SYSTEM: return "WINDOW SYSTEM";
		case GL_DEBUG_SOURCE_SHADER_COMPILER: return "SHADER COMPILER";
		case GL_DEBUG_SOURCE_THIRD_PARTY: return "THIRD PARTY";
		case GL_DEBUG_SOURCE_APPLICATION: return "APPLICATION";
		default: return "OTHER";
		}
	}();

	auto const type_str = [type]() {
		switch (type)
		{
		case GL_DEBUG_TYPE_ERROR: return "ERROR";
		case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR: return "DEPRECATED_BEHAVIOR";
		case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR: return "UNDEFINED_BEHAVIOR";
		case GL_DEBUG_TYPE_PORTABILITY: return "PORTABILITY";
		case GL_DEBUG_TYPE_PERFORMANCE: return "PERFORMANCE";
		case GL_DEBUG_TYPE_MARKER: return "MARKER";
		default: return "OTHER";
		}
	}();

	auto const severity_str = [severity]() {
		switch (severity) {
		case GL_DEBUG_SEVERITY_NOTIFICATION: return "NOTIFICATION";
		case GL_DEBUG_SEVERITY_LOW: return "LOW";
		case GL_DEBUG_SEVERITY_MEDIUM: return "MEDIUM";
		case GL_DEBUG_SEVERITY_HIGH: return "HIGH";
		default: return "OTHER";
		}
	}();
	std::cout << src_str << ", " << type_str << ", " << severity_str << ", " << id << ": " << message << '\n';
}

class image_viewer
{
private:
	GLFWwindow *window;
	glm::ivec2 window_size;

	GLuint null_vaoID;
	GLuint white_texID;
	shader_program program;
	std::unordered_map<int, std::future<image_data>> loading_texdata;
	std::unordered_map<int, GLuint> texture_IDs;  //map {image_index, texture width(scale)} -> gl ID
	std::unordered_map<int, bool> texture_used; //same as prev {} -> used flag

	std::vector<std::string> image_paths;
	std::vector<glm::ivec2> image_sizes;
	std::map<int, std::vector<int>> tags_indices; //tag -> vector of indices pointing to image_paths/image_sizes
	std::unordered_map<int, std::vector<int>> page_numbers; //tag -> vector of indices(referring to tags_indices) indicating the page index

	texture_load_thread loader_pool;

	struct image_pos
	{
		int tag = -1;
		int tag_index = -1;
	} curr_image_pos;

	image_pos advance_pos(image_pos pos, int dir)
	{
		auto tag_it = tags_indices.find(pos.tag);
		if (tag_it == tags_indices.end())
			return {-1, -1};

		int tag_size = tag_it->second.size();
		pos.tag_index += dir;
		if (pos.tag_index == -1 || pos.tag_index == tag_size)
		{
			if ((dir > 0 && std::next(tag_it) == tags_indices.end()) ||
					(dir < 0 && tag_it == tags_indices.begin()))
				pos.tag_index -= dir;
			else
			{
				std::advance(tag_it, dir);
				pos.tag = tag_it->first;
				pos.tag_index = dir > 0 ? 0 : tag_it->second.size() - 1;
			}
		}

		return pos;
	}

	image_pos advance_page(image_pos pos, int dir)
	{
		auto page_numbers_it = page_numbers.find(pos.tag);
		if (page_numbers_it == page_numbers.end())
			return {-1, -1};

		int start_page_number = page_numbers_it->second[pos.tag_index];
		int page_number = start_page_number;
		while (start_page_number == page_number)
		{
			image_pos new_pos = advance_pos(pos, dir);
			if (new_pos.tag == pos.tag && new_pos.tag_index == pos.tag_index)
				return new_pos;

			page_number = page_numbers[new_pos.tag][new_pos.tag_index];
			pos = new_pos;
		}

		return pos;
	}

	int texture_key(int image_index, int width) const
	{
		return width | (image_index << 16);
	}

	void init_GLresources()
	{
		glCreateVertexArrays(1, &null_vaoID);

		program.init("shaders/vert.glsl", "shaders/frag.glsl");
		glm::mat4 proj = glm::ortho(0.f, 800.f, 600.f, 0.f);
		glProgramUniformMatrix4fv(program.id(), 0, 1, 0, &proj[0][0]);
		glViewport(0, 0, 800, 600);

		glCreateTextures(GL_TEXTURE_2D, 1, &white_texID);
		uint8_t white_pixel[] = {255, 255, 255, 255};
		glTextureStorage2D(white_texID, 1, GL_RGBA8, 1, 1);
		glTextureSubImage2D(white_texID, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, white_pixel);
	}

	void on_resize(unsigned int width, unsigned int height)
	{
		glm::mat4 proj = glm::ortho<float>(0.f, width, height, 0.f);
		glProgramUniformMatrix4fv(program.id(), 0, 1, 0, &proj[0][0]);
		glViewport(0, 0, width, height);

		window_size = {width, height};
	}

	void on_key(int key, int action)
	{
		if (action != GLFW_PRESS && action != GLFW_REPEAT)
			return;

		switch (key)
		{
			case GLFW_KEY_SPACE:
				curr_image_pos = advance_page(curr_image_pos, 1);
				break;
			case GLFW_KEY_BACKSPACE:
				curr_image_pos = advance_page(curr_image_pos, -1);
				break;
			case GLFW_KEY_Q:
				glfwSetWindowShouldClose(window, true);
				break;
		}
	}

	void execute_cmd(std::string_view cmd)
	{
		auto arg_start = cmd.find_first_of('(');

		std::string_view type = cmd.substr(0, arg_start);
		std::string_view args_str = cmd.substr(arg_start + 1);
		args_str.remove_suffix(1); //remove last ')'

		std::vector<std::string> args;

		auto comma_pos = std::string_view::npos;
		do
		{
			comma_pos = args_str.find_first_of(',');
			args.emplace_back(args_str.substr(0, comma_pos));
			args_str = args_str.substr(comma_pos + 1);
		} while (comma_pos != std::string_view::npos);

		if (type == "add_image")
		{
			int tag = std::stoi(args[0]);
			auto& tag_indices = tags_indices[tag];

			for (auto image_path : args | std::views::drop(1))
			{
				glm::ivec2 size;
				int ok = stbi_info(image_path.c_str(), &size.x, &size.y, nullptr);
				if (!ok)
				{
					std::cerr << "error loading " << image_path << std::endl;
					continue;
				}

				tag_indices.push_back(image_paths.size());
				image_paths.emplace_back(image_path);
				image_sizes.push_back(size);

				if (curr_image_pos.tag == -1)
				{
					curr_image_pos.tag = tag;
					curr_image_pos.tag_index = tag_indices.back();
				}
			}

			if (tag_indices.empty())
				tags_indices.erase(tag);
			else
				page_numbers[tag] = get_page_numbers(tag_indices);

		}
		else if (type == "goto_offset")
		{
			int offset = std::stoi(args[0]);
			curr_image_pos = advance_page(curr_image_pos, offset);
		}
	}

	void handle_stdin()
	{
		if (std::cin.rdbuf()->in_avail())
		{
			std::string cmd;
			std::getline(std::cin, cmd);
			execute_cmd(cmd);
		}
	}

	void preload_texdata(int image_index, int width)
	{
		int tex_key = texture_key(image_index, width);
		texture_used[tex_key] = true;

		if (texture_IDs.contains(tex_key) || loading_texdata.contains(tex_key))
			return;

		loading_texdata.emplace(tex_key, loader_pool.load_texture(image_paths[image_index], width));
	}

	GLuint get_texture(int image_index, int width)
	{
		int tex_key = texture_key(image_index, width);
		texture_used[tex_key] = true;

		auto tex_it = texture_IDs.find(tex_key);
		if (tex_it != texture_IDs.end())
			return tex_it->second;

		auto data_it = loading_texdata.find(tex_key);
		if (data_it == loading_texdata.end() || data_it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		{
			if (data_it == loading_texdata.end())
				loading_texdata.emplace(tex_key, loader_pool.load_texture(image_paths[image_index], width));

			for (auto& [loaded_key, ID] : texture_IDs)
			{
				int loaded_index = loaded_key >> 16;
				if (image_index == loaded_index)
				{
					texture_used[loaded_key] = true;
					return ID;
				}
			}
			return white_texID;
		}

		image_data loaded_data = data_it->second.get();
		loading_texdata.erase(data_it);

		GLuint& texID = texture_IDs[tex_key];
		glCreateTextures(GL_TEXTURE_2D, 1, &texID);
		glTextureStorage2D(texID, 1, GL_RGBA8, loaded_data.size.x, loaded_data.size.y);
		glTextureSubImage2D(texID, 0, 0, 0, loaded_data.size.x, loaded_data.size.y, GL_RGBA, GL_UNSIGNED_BYTE, loaded_data.pixels.data());

		return texID;
	}

	std::vector<std::pair<int, glm::ivec4>> page_render_data(image_pos pos) const
	{
		auto tag_indices_it = tags_indices.find(pos.tag);
		if (tag_indices_it == tags_indices.end())
			return {};

		auto tag_pages_it = page_numbers.find(pos.tag);
		if (tag_pages_it == page_numbers.end())
			return {};

		const auto& tag_indices = tag_indices_it->second;
		const auto& tag_page_numbers = tag_pages_it->second;

		int page_number = tag_page_numbers[pos.tag_index];

		int page_start_index = pos.tag_index; //initial guess in case already at beginning
		for (int tag_index = page_start_index - 1;; --tag_index)
		{
			if (tag_index < 0 || tag_page_numbers[tag_index] != page_number)
				break;

			page_start_index = tag_index;
		}

		int page_end_index = page_start_index + 1;
		for (int tag_index = page_start_index + 1;; ++tag_index)
		{
			page_end_index = tag_index;
			if (static_cast<unsigned int>(tag_index) == tag_indices.size() || tag_page_numbers[tag_index] != page_number)
				break;
		}

		int start_height = image_sizes[tag_indices[page_start_index]].y;
		glm::vec2 rect_size(0, start_height);

		for (int tag_index = page_start_index; tag_index < page_end_index; ++tag_index)
		{
			glm::ivec2 image_size = image_sizes[tag_indices[tag_index]];
			rect_size.x += image_size.x * start_height / image_size.y;
		}

		float scale_x = window_size.x / static_cast<float>(rect_size.x);
		float scale_y = window_size.y / static_cast<float>(rect_size.y);
		float scale = std::min(scale_x, scale_y);

		glm::ivec2 scaled_size = static_cast<glm::vec2>(rect_size) * scale;
		glm::ivec2 offset = (window_size - scaled_size) / 2;

		std::vector<std::pair<int, glm::ivec4>> sizes_offsets;
		int running_offset = 0;
		for (int tag_index = page_end_index - 1; tag_index >= page_start_index; --tag_index)
		{
			glm::ivec2 image_size = image_sizes[tag_indices[tag_index]];
			int scaled_width = image_size.x * scaled_size.y / image_size.y;

			sizes_offsets.emplace_back(tag_indices[tag_index], glm::ivec4(scaled_width, scaled_size.y, offset.x + running_offset, offset.y));

			running_offset += scaled_width;
		}

		return sizes_offsets;
	}

	void preload_clean_textures()
	{
		for (auto preload_offset : {1, -1})
			for (auto[preload_image_index, size_offset] : page_render_data(advance_page(curr_image_pos, preload_offset)))
				preload_texdata(preload_image_index, size_offset.x);

		for (auto it = texture_used.begin(); it != texture_used.end();)
		{
			auto&[tex_key, used] = *it;
			if (!used)
			{
				auto texID_it = texture_IDs.find(tex_key);
				if (texID_it != texture_IDs.end())
				{
					glDeleteTextures(1, &texID_it->second);
					texture_IDs.erase(tex_key);
				}

				loading_texdata.erase(tex_key);
				it = texture_used.erase(it);
			}
			else
			{
				used = false;
				++it;
			}
		}
		//std::cout << loading_texdata.size() << " " << texture_IDs.size() << std::endl;
	}

	void render()
	{
		for (auto[image_index, size_offset] : page_render_data(curr_image_pos))
		{
			glBindTextureUnit(0, get_texture(image_index, size_offset.x));
			glProgramUniform2f(program.id(), 1, size_offset.z, size_offset.w);
			glProgramUniform2f(program.id(), 2, size_offset.x, size_offset.y);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}

		preload_clean_textures();
	}

public:
	image_viewer() : loader_pool(4)
	{
		if (!glfwInit())
			fprintf(stderr, "ERROR: could not start GLFW3\n");

		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

		window = glfwCreateWindow(800, 600, "PBO test", NULL, NULL);
		if (!window) {
			fprintf(stderr, "ERROR: could not open window with GLFW3\n");
			glfwTerminate();
		}
		window_size = {800, 600};

		glfwMakeContextCurrent(window);
		gl3wInit();

		glfwSwapInterval(1);

		glfwSetWindowUserPointer(window, this);
		glfwSetFramebufferSizeCallback(window , [](GLFWwindow* window, int width, int height) {
			image_viewer* app =
				static_cast<image_viewer*>(glfwGetWindowUserPointer(window));
			app->on_resize(width, height);
		});

		glfwSetKeyCallback(window, [](GLFWwindow* window, int key, int scancode, int action, int mods)
				{
					image_viewer* app =
						static_cast<image_viewer*>(glfwGetWindowUserPointer(window));
					app->on_key(key, action);
				});

		std::ios_base::sync_with_stdio(false);
		std::cin.tie(NULL);

		glEnable(GL_DEBUG_OUTPUT);
		glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
		glDebugMessageCallback(message_callback, nullptr);
		glDebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DEBUG_SEVERITY_NOTIFICATION, 0, nullptr, GL_FALSE);
		init_GLresources();
	}

	void run()
	{
		program.use();
		glBindVertexArray(null_vaoID);
		glClearColor(1.f, 0.f, 0.f, 1.f);

		double t = glfwGetTime();
		while (!glfwWindowShouldClose(window))
		{
			glfwPollEvents();
			handle_stdin();

			glClear(GL_COLOR_BUFFER_BIT);
			render();
			glfwSwapBuffers(window);

			double new_t = glfwGetTime();
			double dt = new_t - t;
			t = new_t;
			//std::cout << 1 / dt << std::endl;
		}
	}

	~image_viewer()
	{
		glDeleteTextures(1, &white_texID);
		for (auto&[tex_key, ID] : texture_IDs)
			glDeleteTextures(1, &ID);
		glDeleteVertexArrays(1, &null_vaoID);

		glfwDestroyWindow(window);
		glfwTerminate();
	}
};
