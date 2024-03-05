#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl3w.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <iostream>
#include <ranges>
#include <filesystem>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>

#include "shader.h"
#include "loader_thread.h"

class image_viewer
{
private:
	GLFWwindow *window;
	glm::ivec2 window_size;

	GLuint null_vaoID;
	GLuint white_texID;
	shader_program program;

	texture_load_thread loader_pool;
	//key for following maps is texture_key(image_index, texture)
	std::unordered_map<int, GLuint> texture_IDs;
	std::unordered_map<int, std::future<image_data>> loading_texdata;
	std::unordered_map<int, bool> texture_used;

	//images vectors
	std::vector<std::string> image_paths;
	std::vector<glm::ivec2> image_sizes;
	std::vector<std::future<glm::ivec2>> loading_image_sizes;
	std::vector<int> image_types;
	std::vector<bool> image_removed;

	//tags maps
	std::map<int, std::vector<int>> tags_indices; //tag -> vector of indices pointing to image vectors
	std::unordered_map<int, std::vector<std::pair<int, std::future<int>>>> loading_image_types;
	std::unordered_map<int, std::vector<int>> page_start_indices; //tag -> vector of indices(referring to tags_indices) indicating the page index
	std::unordered_map<int, bool> update_tag_pages;

	std::vector<int> last_image_indices;

	struct image_pos
	{
		int tag = -1;
		int tag_index = -1;
	} curr_image_pos;

	enum class view_mode
	{
		manga,
		single,
		vertical
	} curr_view_mode;

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
		if (pos.tag_index == -1)
			return {-1, -1};

		int initial_page_start = get_page_start_indices(pos.tag)[pos.tag_index];
		int page_start = initial_page_start;
		while (initial_page_start == page_start)
		{
			image_pos new_pos = advance_pos(pos, dir);
			if (new_pos.tag == pos.tag && new_pos.tag_index == pos.tag_index)
				return new_pos;
			else if (new_pos.tag != pos.tag)
				return new_pos;

			page_start = get_page_start_indices(new_pos.tag)[new_pos.tag_index];
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

		const std::string vert_shader= R"(
#version 460 core

out vec2 fs_texcoords;

layout(location = 0) uniform mat4 proj;
layout(location = 1) uniform vec2 tex_pos;
layout(location = 2) uniform vec2 tex_size;

void main()
{
	const vec2 pos_arr[4] = {{0.0, 0.0}, {0.0, 1.0}, {1.0, 0.0}, {1.0, 1.0}};
	vec2 pos = pos_arr[gl_VertexID];
	fs_texcoords = pos;
	gl_Position = proj * vec4(pos * tex_size + tex_pos, 0.0, 1.0);
})";

		const std::string frag_shader= R"(
#version 460 core
in vec2 fs_texcoords;
out vec4 frag_color;

uniform sampler2D tex;

void main()
{
    frag_color = texture(tex, fs_texcoords);
})";

		program.init(vert_shader, frag_shader);
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

		if (type == "add_images")
		{
			int tag = std::stoi(args[0]);
			auto& tag_indices = tags_indices[tag];

			int prev_curr_image_index = -1;
			if (!tag_indices.empty() && tag == curr_image_pos.tag)
				prev_curr_image_index = tag_indices[curr_image_pos.tag_index];

			for (const auto& image_path : args | std::views::drop(1))
			{
				if (!std::filesystem::exists(image_path))
				{
					std::cerr << image_path << " not found" << std::endl;
					continue;
				}

				int image_index = std::find(image_paths.begin(), image_paths.end(), image_path) - image_paths.begin();

				if (image_index == image_paths.size())
				{
					image_removed.push_back(false);
					image_paths.push_back(image_path);

					image_sizes.emplace_back();
					loading_image_sizes.push_back(loader_pool.get_image_size(image_path));
					image_types.emplace_back();
					loading_image_types[tag].emplace_back(image_index, loader_pool.get_image_type(image_path));
				}
				else if (image_removed[image_index])
					image_removed[image_index] = false;
				else
				{
					std::cerr << image_path << " already present" << std::endl;
					continue;
				}

				tag_indices.push_back(image_index);

				if (curr_image_pos.tag_index == -1)
				{
					curr_image_pos.tag = tag;
					curr_image_pos.tag_index = image_index;
					prev_curr_image_index = image_index;
				}
			}
			std::sort(tag_indices.begin(), tag_indices.end(), [this](int idx1, int idx2)
						{ return image_paths[idx1] < image_paths[idx2]; });

			if (!tag_indices.empty() && tag == curr_image_pos.tag)
				curr_image_pos.tag_index = std::find(tag_indices.begin(), tag_indices.end(), prev_curr_image_index) - tag_indices.begin();

			if (tag_indices.empty())
				tags_indices.erase(tag);
			else
				update_tag_pages[tag] = true;
		}
		else if (type == "goto_tag" || type == "remove_tag")
		{
			int tag = std::stoi(args[0]);
			auto tag_it = tags_indices.find(tag);
			if (tag_it == tags_indices.end())
			{
				std::cerr << "tag " << tag << " not present" << std::endl;
				return;
			}

			if (type == "goto_tag")
			{
				curr_image_pos = {tag, 0};
				return;
			}

			if (tag == curr_image_pos.tag)
			{
				auto new_tag_it = std::next(tag_it);
				if (new_tag_it == tags_indices.end())
				{
					if (tag_it == tags_indices.begin())
						curr_image_pos = {-1, -1};
					else
						new_tag_it = std::prev(tag_it);
				}

				if (curr_image_pos.tag_index != -1)
					curr_image_pos = {new_tag_it->first, 0};
			}

			for (auto image_index : tag_it->second)
				image_removed[image_index] = true;

			tags_indices.erase(tag_it);
			loading_image_types.erase(tag);
			page_start_indices.erase(tag);
			update_tag_pages.erase(tag);
		}
		else if (type == "goto_offset")
		{
			int offset = std::stoi(args[0]);
			curr_image_pos = advance_page(curr_image_pos, offset);
		}
		else if (type == "change_mode")
		{
			std::string new_mode_str = args[0];
			view_mode new_mode;
			if (new_mode_str == "manga")
				new_mode = view_mode::manga;
			else if (new_mode_str == "single")
				new_mode = view_mode::single;
			else if (new_mode_str == "vertical")
				new_mode = view_mode::vertical;
			else
			{
				std::cerr << "mode " << new_mode_str << " not existent" << std::endl;
				return;
			}

			if (new_mode != curr_view_mode)
			{
				std::cout << "current_mode=" << new_mode_str << std::endl;
				curr_view_mode = new_mode;
				for (const auto&[tag, tag_indices] : tags_indices)
					update_tag_pages[tag] = true;
			}
		}
		else if (type == "quit")
		{
			glfwSetWindowShouldClose(window, true);
		}
	}

	void handle_stdin()
	{
		while (std::cin.rdbuf()->in_avail())
		{
			std::string cmd;
			std::getline(std::cin, cmd);
			execute_cmd(cmd);
		}
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

	//this must block,
	glm::ivec2 get_image_size(int image_index)
	{
		auto& future = loading_image_sizes[image_index];
		if (future.valid())
		{
			if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
				image_sizes[image_index] = future.get();
			else
			{
				glm::ivec2 size;
				stbi_info(image_paths[image_index].c_str(), &size.x, &size.y, nullptr);
				image_sizes[image_index] = size;
				future = std::future<glm::ivec2>();
			}
		}

		return image_sizes[image_index];
	}

	std::vector<int>& get_page_start_indices(int tag)
	{
		auto update_pages_it = update_tag_pages.find(tag);
		if (update_pages_it != update_tag_pages.end())
		{
			page_start_indices[tag] = compute_page_start_indices(tags_indices[tag]);
			update_tag_pages.erase(update_pages_it);
		}
		return page_start_indices[tag];
	}

	std::vector<std::pair<int, glm::ivec4>> page_render_data(image_pos pos)
	{
		auto tag_indices_it = tags_indices.find(pos.tag);
		if (tag_indices_it == tags_indices.end())
			return {};

		const auto& tag_indices = tag_indices_it->second;
		const auto& tag_page_starts = get_page_start_indices(pos.tag);

		int page_start_index = tag_page_starts[pos.tag_index];

		int page_end_index;
		for (page_end_index = page_start_index + 1; page_end_index < tag_indices.size(); ++page_end_index)
			if (tag_page_starts[page_end_index] != page_start_index)
				break;

		int start_height = get_image_size(tag_indices[page_start_index]).y;
		glm::vec2 rect_size(0, start_height);

		for (int tag_index = page_start_index; tag_index < page_end_index; ++tag_index)
		{
			glm::ivec2 image_size = get_image_size(tag_indices[tag_index]);
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
			glm::ivec2 image_size = get_image_size(tag_indices[tag_index]);
			int scaled_width = image_size.x * scaled_size.y / image_size.y;

			sizes_offsets.emplace(sizes_offsets.begin(), tag_indices[tag_index], glm::ivec4(scaled_width, scaled_size.y, offset.x + running_offset, offset.y));

			running_offset += scaled_width;
		}

		return sizes_offsets;
	}

	void preload_clean_textures()
	{
		for (auto preload_offset : {1, -1})
			for (auto[preload_image_index, size_offset] : page_render_data(advance_page(curr_image_pos, preload_offset)))
			{
				int tex_key = texture_key(preload_image_index, size_offset.x);
				texture_used[tex_key] = true;

				if (texture_IDs.contains(tex_key) || loading_texdata.contains(tex_key))
					return;

				loading_texdata.emplace(tex_key, loader_pool.load_texture(image_paths[preload_image_index], size_offset.x));
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

	std::vector<int> compute_page_start_indices(const std::vector<int>& indices)
	{
		std::vector<int> tag_page_starts(indices.size());
		if (curr_view_mode != view_mode::manga)
		{
			std::iota(tag_page_starts.begin(), tag_page_starts.end(), 0);
			return tag_page_starts;
		}

		int start = 0;
		int first_alone_score = 0;
		for (unsigned int i = 0; i <= indices.size(); ++i)
		{
			if (i == indices.size() || image_types[indices[i]] == 3)
			{
				first_alone_score += (i < indices.size()) && ((i - start) % 2 == 1);
				first_alone_score -= (i < indices.size()) && ((i - start) % 2 == 0);

				bool first_alone = first_alone_score > 0;

				tag_page_starts[start] = start;
				int page_start = start;
				for (unsigned int j = start + 1; j < i; ++j)
				{
					if ((j - start) % 2 == first_alone)
						page_start = j;

					tag_page_starts[j] = page_start;
				}

				start = i + 1;
				first_alone_score = 0;
				continue;
			}

			int type = image_types[indices[i]];

			first_alone_score -= (type == 1) && ((i - start) % 2 == 0);
			first_alone_score += (type == 1) && ((i - start) % 2 == 1);
			first_alone_score += (type == 2) && ((i - start) % 2 == 0);
			first_alone_score -= (type == 2) && ((i - start) % 2 == 1);
		}

		return tag_page_starts;
	}

	void check_paging_updates(int tag)
	{
		if (curr_view_mode != view_mode::manga)
			return;

		auto loading_types_it = loading_image_types.find(tag);
		if (loading_types_it == loading_image_types.end())
			return;

		auto& loading_types = loading_types_it->second;

		bool update = false;
		for (auto&[image_index, future] : loading_types)
		{
			if (future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			{
				image_types[image_index] = future.get();
				update = true;
			}
		}
		loading_types.erase(std::remove_if(loading_types.begin(), loading_types.end(),
							   [](auto& val){ return !val.second.valid(); }), loading_types.end());

		if (update)
			update_tag_pages[tag] = true;
	}

	void render()
	{
		check_paging_updates(curr_image_pos.tag);

		std::vector<int> current_image_indices;
		for (auto[image_index, size_offset] : page_render_data(curr_image_pos))
		{
			glBindTextureUnit(0, get_texture(image_index, size_offset.x));
			glProgramUniform2f(program.id(), 1, size_offset.z, size_offset.w);
			glProgramUniform2f(program.id(), 2, size_offset.x, size_offset.y);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			current_image_indices.push_back(image_index);
		}

		if (current_image_indices != last_image_indices)
		{
			std::cout << "current_image=";
			for (auto index : current_image_indices)
				std::cout << image_paths[index] << '\t';
			std::cout << std::endl;
		}
		last_image_indices = current_image_indices;

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

		window = glfwCreateWindow(800, 600, "image viewer", NULL, NULL);
		if (!window) {
			fprintf(stderr, "ERROR: could not open window with GLFW3\n");
			glfwTerminate();
		}
		window_size = {800, 600};

		glfwMakeContextCurrent(window);
		gl3wInit();

		glfwSwapInterval(0);

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

		init_GLresources();

		std::ios_base::sync_with_stdio(false);
		std::cin.tie(NULL);

		curr_view_mode = view_mode::manga;
		std::cout << "current_mode=manga" << std::endl;
	}

	void run()
	{
		program.use();
		glBindVertexArray(null_vaoID);
		glClearColor(1.f, 0.f, 0.f, 1.f);

		while (!glfwWindowShouldClose(window))
		{
			double last_t = glfwGetTime();
			glfwPollEvents();
			handle_stdin();

			glClear(GL_COLOR_BUFFER_BIT);
			render();
			glfwSwapBuffers(window);

			std::this_thread::sleep_for(std::chrono::microseconds(int(1000000 * (1.0 / 20 - (glfwGetTime() - last_t)))));
			//double dt = glfwGetTime() - last_t;
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
