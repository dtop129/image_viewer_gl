#define GLFW_INCLUDE_NONE
#include <GL/gl3w.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>

#include "loader_thread.hpp"

class image_viewer {
  private:
	GLFWwindow *window, *load_window;
	glm::ivec2 window_size;

	GLuint null_vaoID;
	GLuint white_tex;
	shader_program program;

	texture_load_pool loader_pool;
	// key for following maps is texture_key(image_index, texture)
	std::unordered_map<int64_t, lazy_load<GLuint>> textures;
	std::unordered_map<int64_t, bool> texture_used;

	// images vectors
	std::vector<std::string> image_paths;
	std::vector<lazy_load<glm::ivec2>> image_sizes;
	std::vector<lazy_load<int>> image_types;
	std::vector<bool> image_removed;
	std::vector<bool> paging_invert;

	// tags maps
	std::map<int, std::vector<int>>
		tags_indices; // tag -> vector of indices pointing to image vectors

	struct image_pos {
		int tag = -1;
		int tag_index = -1;
		bool operator==(image_pos const &) const = default;
	} curr_image_pos;

	std::vector<int> last_image_indices;

	enum class view_mode { manga, single, vertical } curr_view_mode;
	float vertical_offset = 0.f;

	int pressed_key;
	double time_pressed_key = 0.0;
	double repeat_wait = 0.0;

	void init_window() {
		if (!glfwInit())
			fprintf(stderr, "ERROR: could not start GLFW3\n");

		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 6);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

		glfwWindowHint(GLFW_VISIBLE, GLFW_TRUE);
		window =
			glfwCreateWindow(800, 600, "image viewer", nullptr, nullptr);

		glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
		load_window = glfwCreateWindow(1, 1, "load window", nullptr, window);

		if (!window) {
			fprintf(stderr, "ERROR: could not open window with GLFW3\n");
			glfwTerminate();
		}
		window_size = {800, 600};

		glfwMakeContextCurrent(window);
		gl3wInit();

		glfwSwapInterval(1);

		glfwSetWindowUserPointer(window, this);
		glfwSetFramebufferSizeCallback(window, [](GLFWwindow *window, int width,
												  int height) {
			image_viewer *app =
				static_cast<image_viewer *>(glfwGetWindowUserPointer(window));
			app->on_resize(width, height);
		});

		glfwSetKeyCallback(window, [](GLFWwindow *window, int key, int scancode,
									  int action, int mods) {
			image_viewer *app =
				static_cast<image_viewer *>(glfwGetWindowUserPointer(window));
			app->on_key(key, action);
		});

		glfwSetMouseButtonCallback(window, [](GLFWwindow *window, int button,
											  int action, int mods) {
			image_viewer *app =
				static_cast<image_viewer *>(glfwGetWindowUserPointer(window));
			app->on_button(button, action);
		});
	}

	void init_GLresources() {
		glCreateVertexArrays(1, &null_vaoID);

		const std::string vert_shader = R"(
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

		const std::string frag_shader = R"(
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

		uint8_t white_pixel[] = {255, 255, 255, 255};
		glCreateTextures(GL_TEXTURE_2D, 1, &white_tex);
		glTextureStorage2D(white_tex, 1, GL_RGBA8, 1, 1);
		glTextureSubImage2D(white_tex, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE,
							white_pixel);

		loader_pool.init(load_window, std::thread::hardware_concurrency() - 1);
	}

	void on_resize(int width, int height) {
		if (window_size == glm::ivec2(width, height))
			return;

		glm::mat4 proj = glm::ortho<float>(0.f, width, height, 0.f);
		glProgramUniformMatrix4fv(program.id(), 0, 1, 0, &proj[0][0]);
		glViewport(0, 0, width, height);

		window_size = {width, height};
		fix_vertical_limits();
	}

	void on_key(int key, int action) {
		if (action == GLFW_PRESS) {
			pressed_key = key;
			time_pressed_key = glfwGetTime();
			repeat_wait = 0.3;
		}
		if (action == GLFW_RELEASE && key == pressed_key)
			pressed_key = -1;

		if (action == GLFW_PRESS) {
			if (curr_view_mode == view_mode::vertical) {
				switch (key) {
				case GLFW_KEY_SPACE:
				case GLFW_KEY_LEFT:
					vertical_scroll(-200.f);
					fix_vertical_limits();
					break;
				case GLFW_KEY_BACKSPACE:
				case GLFW_KEY_RIGHT:
					vertical_scroll(200.f);
					fix_vertical_limits();
					break;
				}
			} else {
				switch (key) {
				case GLFW_KEY_SPACE:
				case GLFW_KEY_LEFT:
					advance_current_pos(1);
					break;
				case GLFW_KEY_BACKSPACE:
				case GLFW_KEY_RIGHT:
					advance_current_pos(-1);
					break;
				}
			}
		}
		if (action == GLFW_PRESS)
			switch (key) {
			case GLFW_KEY_Q:
				glfwSetWindowShouldClose(window, true);
				break;
			case GLFW_KEY_M:
				change_mode(view_mode::manga);
				break;
			case GLFW_KEY_S:
				change_mode(view_mode::single);
				break;
			case GLFW_KEY_V:
				change_mode(view_mode::vertical);
				break;
			case GLFW_KEY_R:
				if (curr_image_pos.tag_index != -1) {
					int curr_image_index =
						tags_indices[curr_image_pos.tag]
									[curr_image_pos.tag_index];
					paging_invert[curr_image_index] =
						!paging_invert[curr_image_index];
				}
				break;
			case GLFW_KEY_C:
				std::cout << "changechapter" << std::endl;
				break;
			case GLFW_KEY_I:
				std::cout << "getinfo" << std::endl;
				break;
			}
	}

	void on_button(int button, int action) {
		if (action == GLFW_RELEASE)
			return;

		if (action == GLFW_PRESS)
			switch (button) {
			case GLFW_MOUSE_BUTTON_LEFT:
				advance_current_pos(1);
				break;
			case GLFW_MOUSE_BUTTON_RIGHT:
				advance_current_pos(-1);
				break;
			case GLFW_MOUSE_BUTTON_MIDDLE:
				if (curr_image_pos.tag_index != -1) {
					int curr_image_index =
						tags_indices[curr_image_pos.tag]
									[curr_image_pos.tag_index];
					paging_invert[curr_image_index] =
						!paging_invert[curr_image_index];
				}
				break;
			}
	}

	void handle_keys(float dt) {
		float offset = 1000 * dt;
		switch (pressed_key) {
		case GLFW_KEY_J:
		case GLFW_KEY_DOWN:
			vertical_scroll(-offset);
			fix_vertical_limits();
			break;
		case GLFW_KEY_K:
		case GLFW_KEY_UP:
			vertical_scroll(offset);
			fix_vertical_limits();
			break;
		case GLFW_KEY_SPACE:
		case GLFW_KEY_LEFT:
			if (glfwGetTime() - time_pressed_key > repeat_wait)
			{
				advance_current_pos(1);
				time_pressed_key = glfwGetTime();
				repeat_wait = 0.05;
			}
			break;
		case GLFW_KEY_BACKSPACE:
		case GLFW_KEY_RIGHT:
			if (glfwGetTime() - time_pressed_key > repeat_wait)
			{
				advance_current_pos(-1);
				time_pressed_key = glfwGetTime();
				repeat_wait = 0.05;
			}
			break;
		default:
			return;
		}
	}

	bool set_curr_image_pos(image_pos new_pos) {
		if (new_pos == curr_image_pos || new_pos.tag_index == -1)
			return false;

		curr_image_pos = new_pos;
		vertical_offset = 0.f;
		return true;
	}

	void preload_close_image_types() {
		auto tag_it = tags_indices.find(curr_image_pos.tag);
		if (tag_it == tags_indices.end() || curr_view_mode != view_mode::manga)
			return;

		image_pos pos = curr_image_pos;
		for (int i = 0; i < 10; i++) {
			pos.tag_index += 1;
			if (pos.tag_index == tag_it->second.size()) {
				std::advance(tag_it, 1);
				if (tag_it == tags_indices.end())
					return;

				pos = {tag_it->first, 0};
			}

			int image_index = tags_indices[pos.tag][pos.tag_index];
			if (!image_types[image_index].has_value())
				std::tie(image_sizes[image_index], image_types[image_index]) =
					loader_pool.get_size_type(image_paths[image_index]);
		}
	}

	bool advance_current_pos(int dir) {
		if (curr_image_pos.tag_index == -1)
			return false;

		image_pos start_pos = curr_image_pos;
		float start_vertical_offset = vertical_offset;

		if (curr_view_mode == view_mode::vertical)
			if (start_vertical_offset < 0 && dir < 1) {
				vertical_offset = 0.f;
				return false;
			}

		set_curr_image_pos(try_advance_pos(curr_image_pos, dir));
		fix_vertical_limits();

		if (start_pos == curr_image_pos) {
			std::cout << "last_in_dir=" << dir << std::endl;
			return false;
		}

		return true;
	}

	image_pos try_advance_pos(image_pos pos, int dir) {
		auto tag_it = tags_indices.find(pos.tag);
		if (tag_it == tags_indices.end())
			return {-1, -1};

		auto page_start_indices = get_page_start_indices(tag_it->second);
		int initial_page_start = page_start_indices[pos.tag_index];
		int page_start = initial_page_start;
		while (initial_page_start == page_start) {
			pos.tag_index += dir;
			if (pos.tag_index == tag_it->second.size() || pos.tag_index == -1) {
				if ((dir > 0 && std::next(tag_it) == tags_indices.end()) ||
					(dir < 0 && tag_it == tags_indices.begin()))
					return {-1, -1};

				std::advance(tag_it, dir);
				pos = {tag_it->first,
					   int(dir > 0 ? 0 : tag_it->second.size() - 1)};
				page_start_indices = get_page_start_indices(tag_it->second);
				page_start = page_start_indices[pos.tag_index];
				break;
			}
			page_start = page_start_indices[pos.tag_index];
		}
		if (initial_page_start == page_start)
			return {-1, -1};

		return {pos.tag, page_start};
	}

	glm::ivec2 get_image_size(int image_index) {
		return image_sizes[image_index].get_or({1000, 1414});
	}

	int get_image_type(int image_index) {
		return image_types[image_index].get_or(0);
	}

	int64_t texture_key(int image_index, glm::ivec2 size) const {
		int64_t key = image_index;
		key = (key << 16) | size.x;
		return (key << 16) | size.y;
	}

	void execute_cmd(std::string_view cmd) {
		auto arg_start = cmd.find_first_of('(');

		std::string_view type = cmd.substr(0, arg_start);
		std::string_view args_str = cmd.substr(arg_start + 1);
		args_str.remove_suffix(1); // remove last ')'

		std::vector<std::string> args;

		auto comma_pos = std::string_view::npos;
		do {
			comma_pos = args_str.find_first_of(',');
			args.emplace_back(args_str.substr(0, comma_pos));
			args_str = args_str.substr(comma_pos + 1);
		} while (comma_pos != std::string_view::npos);

		if (type == "add_images") {
			int tag = std::stoi(args[0]);
			auto &tag_indices = tags_indices[tag];

			int prev_curr_image_index = -1;
			if (!tag_indices.empty() && tag == curr_image_pos.tag)
				prev_curr_image_index = tag_indices[curr_image_pos.tag_index];

			for (const auto &image_path : args | std::views::drop(1)) {
				if (!std::filesystem::exists(image_path)) {
					std::cerr << image_path << " not found" << std::endl;
					continue;
				}

				int image_index = std::find(image_paths.begin(),
											image_paths.end(), image_path) -
								  image_paths.begin();

				if (image_index == image_paths.size()) {
					image_removed.push_back(false);
					image_paths.push_back(image_path);
					paging_invert.push_back(false);

					image_sizes.emplace_back();
					image_types.emplace_back();
				} else if (image_removed[image_index])
					image_removed[image_index] = false;
				else {
					std::cerr << image_path << " already present" << std::endl;
					continue;
				}

				tag_indices.push_back(image_index);
			}
			if (tag_indices.empty()) {
				tags_indices.erase(tag);
				return;
			}

			if (curr_image_pos.tag_index == -1) {
				set_curr_image_pos({tag, 0});
				prev_curr_image_index = tag_indices.front();
			}
			std::sort(tag_indices.begin(), tag_indices.end(),
					  [this](int idx1, int idx2) {
						  return image_paths[idx1] < image_paths[idx2];
					  });

			if (curr_image_pos.tag == tag) {
				int correct_tag_index =
					std::find(tag_indices.begin(), tag_indices.end(),
							  prev_curr_image_index) -
					tag_indices.begin();

				curr_image_pos = {tag, correct_tag_index};
			}
		} else if (type == "goto_tag" || type == "remove_tag") {
			int tag = std::stoi(args[0]);
			auto tag_it = tags_indices.find(tag);
			if (tag_it == tags_indices.end()) {
				std::cerr << "tag " << tag << " not present" << std::endl;
				return;
			}

			if (type == "goto_tag") {
				set_curr_image_pos({tag, 0});
				return;
			}

			if (tag == curr_image_pos.tag) {
				auto new_tag_it = std::next(tag_it);
				if (new_tag_it == tags_indices.end()) {
					if (tag_it == tags_indices.begin())
						curr_image_pos = {-1, -1};
					else
						new_tag_it = std::prev(tag_it);
				}

				if (curr_image_pos.tag_index != -1)
					set_curr_image_pos({new_tag_it->first, 0});
			}

			for (auto image_index : tag_it->second)
				image_removed[image_index] = true;

			tags_indices.erase(tag_it);
		} else if (type == "change_mode") {
			std::string new_mode_str = args[0];
			view_mode new_mode;
			if (new_mode_str == "manga")
				new_mode = view_mode::manga;
			else if (new_mode_str == "single")
				new_mode = view_mode::single;
			else if (new_mode_str == "vertical")
				new_mode = view_mode::vertical;
			else {
				std::cerr << "mode " << new_mode_str << " not existent"
						  << std::endl;
				return;
			}
			change_mode(new_mode);
		} else if (type == "quit")
			glfwSetWindowShouldClose(window, true);
	}

	void change_mode(view_mode new_mode) {
		if (new_mode == curr_view_mode)
			return;

		std::string new_mode_str = [new_mode] {
			switch (new_mode) {
			case view_mode::manga:
				return "manga";
			case view_mode::single:
				return "single";
			case view_mode::vertical:
				return "vertical";
			}
		}();
		std::cout << "current_mode=" << new_mode_str << std::endl;
		curr_view_mode = new_mode;
	}

	void fix_vertical_limits() {
		if (curr_view_mode != view_mode::vertical)
			return;

		auto new_render_data = get_current_vertical_strip();
		if (new_render_data.empty())
			return;

		auto &[last_pos, last_size_offset] = new_render_data.back();
		float bottom_edge = last_size_offset.w + last_size_offset.y;

		if (bottom_edge < window_size.y) {
			vertical_offset -= bottom_edge - window_size.y;
			new_render_data = get_current_vertical_strip();
		}

		auto &[first_pos, first_size_offset] = new_render_data.front();
		float upper_edge = first_size_offset.w;

		curr_image_pos = first_pos; // here we set the vertical offset later,
									// need to call set_curr_image_pos
		vertical_offset = upper_edge > 0.f ? 0.f : first_size_offset.w;
	}

	void vertical_scroll(float offset) {
		if (curr_view_mode != view_mode::vertical)
			return;

		image_pos start_pos = curr_image_pos;
		float start_offset = vertical_offset;
		vertical_offset += offset;

		fix_vertical_limits();
		if (offset != 0 && start_pos == curr_image_pos &&
			start_offset == vertical_offset) {
			std::cout << "last_in_dir=" << (offset < 0.f) - (offset > 0.f)
					  << std::endl;
		}
	}

	void handle_stdin() {
		while (std::cin.rdbuf()->in_avail()) {
			std::string cmd;
			std::getline(std::cin, cmd);
			execute_cmd(cmd);
		}
	}

	GLuint get_texture(int image_index, glm::ivec2 size) {
		int64_t tex_key = texture_key(image_index, size);
		texture_used[tex_key] = true;

		if (!image_sizes[image_index].has_value())
			std::tie(image_sizes[image_index], image_types[image_index]) =
				loader_pool.get_size_type(image_paths[image_index]);

		if (!image_sizes[image_index].ready())
			return white_tex;

		if (!textures.contains(tex_key))
			textures.try_emplace(tex_key, loader_pool.load_texture(
											  image_paths[image_index], size));

		GLuint tex = textures[tex_key].get_or(white_tex);
		if (tex == white_tex) {
			auto loaded_it = std::find_if(
				textures.begin(), textures.end(), [image_index](auto &tex) {
					return tex.first >> 32 == image_index && tex.second.ready();
				});
			if (loaded_it != textures.end()) {
				texture_used[loaded_it->first] = true;
				return loaded_it->second.get();
			}
		}
		return tex;
	}

	std::vector<int> get_page_start_indices(const std::vector<int> &indices) {
		std::vector<int> tag_page_starts(indices.size());
		if (curr_view_mode == view_mode::single ||
			curr_view_mode == view_mode::vertical) {
			std::iota(tag_page_starts.begin(), tag_page_starts.end(), 0);
			return tag_page_starts;
		}

		int start = 0;
		int first_alone_score = 0;
		bool invert_alone = false;
		for (unsigned int i = 0; i <= indices.size(); ++i) {
			int type = (i == indices.size()) ? 3 : get_image_type(indices[i]);
			if (type == 3) {
				first_alone_score +=
					(i < indices.size()) && ((i - start) % 2 == 1);
				first_alone_score -=
					(i < indices.size()) && ((i - start) % 2 == 0);

				bool first_alone = (first_alone_score > 0) ^ invert_alone;

				if (start < indices.size())
					tag_page_starts[start] = start;
				int page_start = start;
				for (unsigned int j = start; j < i; ++j) {
					if ((j - start) % 2 == first_alone || j == start)
						page_start = j;

					tag_page_starts[j] = page_start;
				}
				if (i != indices.size())
					tag_page_starts[i] = i;

				start = i + 1;
				first_alone_score = 0;
				invert_alone = false;
				continue;
			}

			first_alone_score -= (type == 1) && ((i - start) % 2 == 0);
			first_alone_score += (type == 1) && ((i - start) % 2 == 1);
			first_alone_score += (type == 2) && ((i - start) % 2 == 0);
			first_alone_score -= (type == 2) && ((i - start) % 2 == 1);

			if (paging_invert[indices[i]])
				invert_alone = !invert_alone;
		}

		return tag_page_starts;
	}

	glm::vec4 vertical_slice_center(int image_index) {
		float strip_width = std::min(600.f, window_size.x * 0.8f);
		glm::vec2 image_size = get_image_size(image_index);
		glm::vec2 scaled_size(strip_width,
							  image_size.y * strip_width / image_size.x);
		return glm::round(
			glm::vec4(scaled_size, (window_size.x - strip_width) * 0.5f, 0.f));
	}

	std::vector<std::pair<image_pos, glm::vec4>> center_page(image_pos pos) {
		if (pos.tag_index == -1)
			return {};
		if (curr_view_mode == view_mode::vertical)
			return {{pos, vertical_slice_center(
							  tags_indices[pos.tag][pos.tag_index])}};

		const auto &tag_indices = tags_indices[pos.tag];

		int image_index = tag_indices[pos.tag_index];
		glm::vec2 start_image_size = get_image_size(image_index);

		const auto tag_page_starts = get_page_start_indices(tag_indices);

		int page_start_index = tag_page_starts[pos.tag_index];

		int page_end_index;
		for (page_end_index = page_start_index + 1;
			 page_end_index < tag_indices.size(); ++page_end_index)
			if (tag_page_starts[page_end_index] != page_start_index)
				break;

		float start_height = start_image_size.y;
		glm::vec2 rect_size(0, start_height);

		for (int tag_index = page_start_index; tag_index < page_end_index;
			 ++tag_index) {
			glm::vec2 image_size = get_image_size(tag_indices[tag_index]);
			rect_size.x += image_size.x * start_height / image_size.y;
		}

		float scale_x = window_size.x / static_cast<float>(rect_size.x);
		float scale_y = window_size.y / static_cast<float>(rect_size.y);
		float scale = std::min(scale_x, scale_y);

		glm::vec2 scaled_rect_size = rect_size * scale;
		glm::vec2 offset =
			glm::round((glm::vec2(window_size) - scaled_rect_size) * 0.5f);

		std::vector<std::pair<image_pos, glm::vec4>> sizes_offsets;
		float running_offset = 0;
		for (int tag_index = page_end_index - 1; tag_index >= page_start_index;
			 --tag_index) {
			glm::vec2 image_size = get_image_size(tag_indices[tag_index]);
			float scaled_width =
				image_size.x * scaled_rect_size.y / image_size.y;
			glm::vec2 scaled_size(scaled_width, scaled_rect_size.y);

			sizes_offsets.emplace(
				sizes_offsets.begin(), image_pos(pos.tag, tag_index),
				glm::round(glm::vec4(scaled_size, offset.x + running_offset,
									 offset.y)));

			running_offset += sizes_offsets.back().second.x;
		}

		return sizes_offsets;
	}

	std::vector<std::pair<image_pos, glm::vec4>> get_current_vertical_strip() {
		std::vector<std::pair<image_pos, glm::vec4>> sizes_offsets;
		image_pos pos = curr_image_pos;
		float offset_y = glm::round(vertical_offset);

		image_pos prev_pos = try_advance_pos(pos, -1);
		while (offset_y > 0 && prev_pos.tag_index != -1) {
			pos = prev_pos;
			glm::vec4 scaled_size_offset =
				vertical_slice_center(tags_indices[pos.tag][pos.tag_index]);

			offset_y -= scaled_size_offset.y;
			prev_pos = try_advance_pos(pos, -1);
		}
		while (offset_y < window_size.y && pos.tag_index != -1) {
			glm::vec4 scaled_size_offset =
				vertical_slice_center(tags_indices[pos.tag][pos.tag_index]);
			scaled_size_offset.w += offset_y;

			if (offset_y + scaled_size_offset.y > 0)
				sizes_offsets.emplace_back(pos, scaled_size_offset);

			offset_y += scaled_size_offset.y;
			pos = try_advance_pos(pos, 1);
		}

		return sizes_offsets;
	}

	std::vector<std::pair<image_pos, glm::vec4>> get_current_render_data() {
		std::vector<std::pair<image_pos, glm::vec4>> sizes_offsets;
		if (curr_image_pos.tag_index == -1)
			return sizes_offsets;

		if (curr_view_mode == view_mode::vertical)
			sizes_offsets = get_current_vertical_strip();
		else
			sizes_offsets = center_page(curr_image_pos);

		std::array<std::pair<image_pos, int>, 2> preload_offsets = {
			std::pair{sizes_offsets.front().first, -1},
			std::pair{sizes_offsets.back().first, 1}};

		for (auto [start, offset] : preload_offsets)
			for (auto [pos, size_offset] :
				 center_page(try_advance_pos(start, offset)))
				sizes_offsets.emplace_back(
					pos,
					glm::vec4(size_offset.x, size_offset.y, 1000000, 1000000));

		return sizes_offsets;
	}

	void render() {
		preload_close_image_types();
		auto current_render_data = get_current_render_data();

		std::vector<int> current_image_indices;
		for (auto [pos, size_offset] : current_render_data) {
			int image_index = tags_indices[pos.tag][pos.tag_index];
			GLuint tex =
				get_texture(image_index, {size_offset.x, size_offset.y});
			if (size_offset.z == 1000000) // preload
				continue;

			glBindTextureUnit(0, tex);
			glProgramUniform2f(program.id(), 1, size_offset.z, size_offset.w);
			glProgramUniform2f(program.id(), 2, size_offset.x, size_offset.y);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			current_image_indices.push_back(image_index);
		}

		for (auto &[key, used] : texture_used)
			if (!used) {
				glDeleteTextures(1, &textures[key].get_or(0));
				textures.erase(key);
			}
		std::erase_if(texture_used,
					  [](auto &pair) { return pair.second == false; });
		for (auto &[key, used] : texture_used)
			used = false;

		if (current_image_indices != last_image_indices) {
			std::cout << "current_image=";
			for (auto index : current_image_indices)
				std::cout << image_paths[index] << '\t';
			std::cout << std::endl;
		}
		last_image_indices = current_image_indices;
	}

  public:
	image_viewer() {
		init_window();
		init_GLresources();
	}

	void run() {
		std::ios_base::sync_with_stdio(false);
		std::cin.tie(NULL);

		curr_view_mode = view_mode::manga;

		program.use();
		glBindVertexArray(null_vaoID);
		glClearColor(0.f, 0.f, 0.f, 1.f);

		std::cout << "current_mode=manga" << std::endl;

		double dt = 0;
		int i = 0;
		while (!glfwWindowShouldClose(window)) {
			double last_t = glfwGetTime();
			glfwPollEvents();
			handle_stdin();
			handle_keys(dt);

			glClear(GL_COLOR_BUFFER_BIT);
			if (i++ > 5)
				render();
			glfwSwapBuffers(window);

			dt = glfwGetTime() - last_t;
		}
	}

	~image_viewer() {
		glDeleteTextures(1, &white_tex);
		for (auto &[key, tex] : textures)
			glDeleteTextures(1, &tex.get());
		glDeleteVertexArrays(1, &null_vaoID);
		program.destroy();
		loader_pool.destroy();

		glfwDestroyWindow(load_window);
		glfwDestroyWindow(window);
		glfwTerminate();
	}
};
