#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl3w.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define BS_THREAD_POOL_ENABLE_PRIORITY
#include <BS_thread_pool.hpp>
#include <lancir.h>

#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <poll.h>

#include "shader.h"

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

struct image_data
{
	std::vector<uint8_t> pixels;
	glm::ivec2 size;
};

image_data load_image(const std::string& path, int desired_width)
{
	image_data data;

	int width, height;
	uint8_t* pixels = stbi_load(path.c_str(), &width, &height, nullptr, 3);

	int desired_height = height * desired_width / width;
	data.size = {desired_width, desired_height};
	std::vector<uint8_t> resized_pixels(desired_width * desired_height * 3);

	avir::CLancIR resizer;
	resizer.resizeImage(pixels, width, height, resized_pixels.data(), desired_width, desired_height, 3);

	data.pixels.reserve(desired_width * desired_height * 4);
	for (int i = 0; i < resized_pixels.size(); i+=3)
	{
		data.pixels.push_back(resized_pixels[i]);
		data.pixels.push_back(resized_pixels[i+1]);
		data.pixels.push_back(resized_pixels[i+2]);
		data.pixels.push_back(0);
	}

	stbi_image_free(pixels);

	return data;
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
	std::unordered_map<int, std::vector<int>> page_markers; //tag -> vector of indices(referring to tags_indices) indicating the start of a page

	BS::thread_pool loader_pool;

	//std::vector<lazy_load<unsigned short>> image_type; //for manga paging(0 undefined, 1 left page, 2 right page, 3 wide page)

	struct image_pos
	{
		int tag = -1;
		int tag_index = -1;
	} curr_image_pos;

	image_pos advance_pos(image_pos pos, int offset)
	{
		auto tag_iter = tags_indices.find(pos.tag);
		if (tag_iter == tags_indices.end())
			return {-1, -1};

		int tag_size = tag_iter->second.size();
		while (offset != 0)
		{
			int tag_offset = std::clamp(pos.tag_index + offset, 0, tag_size - 1) - pos.tag_index;
			offset = offset - tag_offset;
			pos.tag_index += tag_offset;

			if (offset != 0)
			{
				int offset_dir = (offset > 0) - (offset < 0);
				if ((offset > 0 && std::next(tag_iter) == tags_indices.end()) ||
						(offset < 0 && tag_iter == tags_indices.begin()))
					break;

				std::advance(tag_iter, offset_dir);
				tag_size = tag_iter->second.size();
				offset -= offset_dir;

				pos.tag = tag_iter->first;
				pos.tag_index = offset_dir > 0 ? 0 : tag_size - 1;
			}
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
				curr_image_pos = advance_pos(curr_image_pos, 1);
				break;
			case GLFW_KEY_BACKSPACE:
				curr_image_pos = advance_pos(curr_image_pos, -1);
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
		}
		else if (type == "goto_offset")
		{
			int offset = std::stoi(args[0]);
			curr_image_pos = advance_pos(curr_image_pos, offset);
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

		loading_texdata.emplace(tex_key, loader_pool.submit_task([&path = image_paths[image_index], width](){ return load_image(path, width); }));
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
			preload_texdata(image_index, width);

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

	std::pair<glm::ivec2, glm::ivec2> get_centering_scale_offset(int image_index)
	{
		glm::ivec2 image_size = image_sizes[image_index];

		float scale_x = window_size.x / static_cast<float>(image_size.x);
		float scale_y = window_size.y / static_cast<float>(image_size.y);

		float scale = std::min(scale_x, scale_y);

		glm::ivec2 scaled_size = static_cast<glm::vec2>(image_size) * scale;
		glm::ivec2 offset = (window_size - scaled_size) / 2;

		return {scaled_size, offset};
	}

	void render()
	{
		auto curr_tag_it = tags_indices.find(curr_image_pos.tag);
		if (curr_tag_it == tags_indices.end())
			return;


		const auto& curr_tag_indices = curr_tag_it->second;
		int image_index = curr_tag_indices[curr_image_pos.tag_index];

		auto[scaled_size, offset] = get_centering_scale_offset(image_index);
		GLuint texID = get_texture(image_index, scaled_size.x);

		glProgramUniform2f(program.id(), 1, offset.x, offset.y);
		glProgramUniform2f(program.id(), 2, scaled_size.x, scaled_size.y);
		glBindTextureUnit(0, texID);

		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

		for (auto preload_offset : {1, -1})
		{
			image_pos preload_pos = advance_pos(curr_image_pos, preload_offset);
			int preload_image_index = tags_indices[preload_pos.tag][preload_pos.tag_index];
			auto[scaled_size, offset] = get_centering_scale_offset(preload_image_index);
			preload_texdata(preload_image_index, scaled_size.x);
		}

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

public:
	image_viewer()
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
