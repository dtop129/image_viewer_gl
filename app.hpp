#define GLFW_INCLUDE_NONE
#include <GL/gl3w.h>
#include <GLFW/glfw3.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <filesystem>
#include <iostream>
#include <map>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>

#include "loader_thread.hpp"
#include "shader.hpp"

class image_viewer {
  private:
	GLFWwindow *window;
	glm::ivec2 window_size;
	std::unordered_map<int, bool> keys_pressed;

	GLuint null_vaoID;
	GLuint white_texID;
	shader_program program;

	texture_load_thread loader_pool;
	// key for following maps is texture_key(image_index, texture)
	std::unordered_map<int, lazy_load<image_data, GLuint>> texture_IDs;
	std::unordered_map<int, bool> texture_used;

	// images vectors
	std::vector<std::string> image_paths;
	std::vector<lazy_load<glm::ivec2>> image_sizes;
	std::vector<lazy_load<int>> image_types;
	std::vector<bool> image_removed;
	std::vector<bool> paging_invert;

	// tags maps
	std::map<int, std::vector<int>>
		tags_indices; // tag -> vector of indices pointing to image vectors

	std::vector<int> last_image_indices;

	struct image_pos {
		int tag = -1;
		int tag_index = -1;
		bool operator==(image_pos const &) const = default;
	} curr_image_pos;

	enum class view_mode { manga, single, vertical } curr_view_mode;

	float vertical_offset = 0.f;

	void init_window() {
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

		glCreateTextures(GL_TEXTURE_2D, 1, &white_texID);
		uint8_t white_pixel[] = {255, 255, 255, 255};
		glTextureStorage2D(white_texID, 1, GL_RGBA8, 1, 1);
		glTextureSubImage2D(white_texID, 0, 0, 0, 1, 1, GL_RGBA,
							GL_UNSIGNED_BYTE, white_pixel);
	}

	void on_resize(unsigned int width, unsigned int height) {
		glm::mat4 proj = glm::ortho<float>(0.f, width, height, 0.f);
		glProgramUniformMatrix4fv(program.id(), 0, 1, 0, &proj[0][0]);
		glViewport(0, 0, width, height);

		window_size = {width, height};
	}

	void on_key(int key, int action) {
		if (action == GLFW_PRESS)
			keys_pressed[key] = true;
		else if (action == GLFW_RELEASE)
			keys_pressed[key] = false;

		if (action == GLFW_PRESS || action == GLFW_REPEAT)
			switch (key) {
			case GLFW_KEY_SPACE:
				advance_current_pos(1);
				break;
			case GLFW_KEY_BACKSPACE:
				advance_current_pos(-1);
				break;
			}
		if (action == GLFW_PRESS)
			switch (key) {
			case GLFW_KEY_Q:
				glfwSetWindowShouldClose(window, true);
				break;
			case GLFW_KEY_C:
				std::cout << "changechapter" << std::endl;
				break;
			case GLFW_KEY_I:
				std::cout << "getinfo" << std::endl;
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
			}
	}

	bool advance_current_pos(int dir) {
		if (curr_image_pos.tag_index == -1)
			return false;

		image_pos new_pos = try_advance_pos(curr_image_pos, dir);

		if (new_pos == curr_image_pos) {
			std::cout << "last_in_dir=" << dir << std::endl;
			return false;
		}

		curr_image_pos = new_pos;
		return true;
	}

	image_pos try_advance_pos(image_pos pos, int dir) {
		auto tag_it = tags_indices.find(pos.tag);
		if (tag_it == tags_indices.end())
			return {-1, -1};

		image_pos start_pos = pos;
		auto page_start_indices = get_page_start_indices(tag_it->second);
		int initial_page_start = page_start_indices[pos.tag_index];
		int page_start = initial_page_start;
		while (initial_page_start == page_start) {
			pos.tag_index += dir;
			if (pos.tag_index == tag_it->second.size() || pos.tag_index == -1) {
				if ((dir > 0 && std::next(tag_it) == tags_indices.end()) ||
					(dir < 0 && tag_it == tags_indices.begin()))
					return start_pos;

				std::advance(tag_it, dir);
				pos = {tag_it->first,
					   int(dir > 0 ? 0 : tag_it->second.size() - 1)};
				page_start_indices = get_page_start_indices(tag_it->second);
				break;
			}
			page_start = page_start_indices[pos.tag_index];
		}

		return {pos.tag, page_start_indices[pos.tag_index]};
	}

	glm::ivec2 get_image_size(int image_index) {
		if (!image_sizes[image_index].has_value()) {
			image_sizes[image_index] =
				lazy_load(loader_pool.get_image_size(image_paths[image_index]));
		}
		return image_sizes[image_index].get();
	}

	int get_image_type(int image_index) {
		if (!image_types[image_index].has_value())
			image_types[image_index] =
				lazy_load(loader_pool.get_image_type(image_paths[image_index]));
		return image_types[image_index].get_or(0);
	}

	int texture_key(int image_index, int width) const {
		return width | (image_index << 16);
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

			// if we add the images not in reverse, the first image loading size
			// will be last in the queue
			for (const auto &image_path :
				 args | std::views::drop(1) | std::views::reverse) {
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

					image_sizes.emplace_back(
						loader_pool.get_image_size(image_path));
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
			if (!tag_indices.empty() && curr_image_pos.tag_index == -1) {
				curr_image_pos.tag = tag;
				prev_curr_image_index = tag_indices.back();
			}
			std::sort(tag_indices.begin(), tag_indices.end(),
					  [this](int idx1, int idx2) {
						  return image_paths[idx1] < image_paths[idx2];
					  });
			if (!tag_indices.empty() && tag == curr_image_pos.tag)
				curr_image_pos.tag_index =
					std::find(tag_indices.begin(), tag_indices.end(),
							  prev_curr_image_index) -
					tag_indices.begin();

			if (tag_indices.empty())
				tags_indices.erase(tag);
		} else if (type == "goto_tag" || type == "remove_tag") {
			int tag = std::stoi(args[0]);
			auto tag_it = tags_indices.find(tag);
			if (tag_it == tags_indices.end()) {
				std::cerr << "tag " << tag << " not present" << std::endl;
				return;
			}

			if (type == "goto_tag") {
				curr_image_pos = {tag, 0};
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
					curr_image_pos = {new_tag_it->first, 0};
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

			if (new_mode != curr_view_mode) {
				std::cout << "current_mode=" << new_mode_str << std::endl;
				curr_view_mode = new_mode;
			}
		} else if (type == "quit")
			glfwSetWindowShouldClose(window, true);
	}

	void handle_keys(float dt) {
		if (curr_image_pos.tag_index == -1)
			return;
		if (curr_view_mode != view_mode::vertical)
			return;

		if (keys_pressed[GLFW_KEY_J])
			vertical_offset -= 500 * dt;
		if (keys_pressed[GLFW_KEY_K])
			vertical_offset += 500 * dt;

		int current_scaled_height =
			vertical_slice_center(
				tags_indices[curr_image_pos.tag][curr_image_pos.tag_index])
				.y;

		if (vertical_offset > 0.f) {
			bool changed = advance_current_pos(-1);
			if (changed) {
				glm::ivec4 size_offset = vertical_slice_center(
					tags_indices[curr_image_pos.tag][curr_image_pos.tag_index]);
				vertical_offset -= size_offset.y;
			} else
				vertical_offset = 0.f;
		} else if (vertical_offset < -current_scaled_height) {
			bool changed = advance_current_pos(1);
			if (changed)
				vertical_offset += current_scaled_height;
			else
				vertical_offset = -current_scaled_height;
		}
	}

	void handle_stdin() {
		while (std::cin.rdbuf()->in_avail()) {
			std::string cmd;
			std::getline(std::cin, cmd);
			execute_cmd(cmd);
		}
	}

	auto &preload_texture(int image_index, int width) {
		int tex_key = texture_key(image_index, width);
		texture_used[tex_key] = true;

		auto tex_it = texture_IDs.find(tex_key);
		if (tex_it != texture_IDs.end())
			return tex_it->second;

		return texture_IDs
			.try_emplace(tex_key,
					 loader_pool.load_texture(image_paths[image_index], width),
					 [](auto &&loaded_data) {
						 GLuint texID;
						 glCreateTextures(GL_TEXTURE_2D, 1, &texID);
						 glTextureStorage2D(texID, 1, GL_RGBA8,
											loaded_data.size.x,
											loaded_data.size.y);
						 glTextureSubImage2D(texID, 0, 0, 0, loaded_data.size.x,
											 loaded_data.size.y, GL_RGBA,
											 GL_UNSIGNED_BYTE,
											 loaded_data.pixels.data());
						 return texID;
					 })
			.first->second;
	}

	GLuint get_texture(int image_index, int width) {
		auto &tex = preload_texture(image_index, width);
		return tex.get_or(white_texID);
	}

	glm::ivec4 vertical_slice_center(int image_index) {
		int strip_width = std::min(600, window_size.x * 4 / 5);
		glm::ivec2 image_size = get_image_size(image_index);
		return {strip_width, image_size.y * strip_width / image_size.x,
				(window_size.x - strip_width) / 2, 0};
	}

	std::vector<std::pair<image_pos, glm::ivec4>> center_page(image_pos pos) {
		if (curr_view_mode == view_mode::vertical)
			return {{pos, vertical_slice_center(
							  tags_indices[pos.tag][pos.tag_index])}};

		const auto &tag_indices = tags_indices[pos.tag];

		int image_index = tag_indices[pos.tag_index];
		glm::ivec2 start_image_size = get_image_size(image_index);
		if (!image_types[image_index].ready() &&
			start_image_size.x > start_image_size.y * 0.8)
			image_types[image_index] = lazy_load(3);

		const auto tag_page_starts = get_page_start_indices(tag_indices);

		int page_start_index = tag_page_starts[pos.tag_index];

		int page_end_index;
		for (page_end_index = page_start_index + 1;
			 page_end_index < tag_indices.size(); ++page_end_index)
			if (tag_page_starts[page_end_index] != page_start_index)
				break;

		int start_height = start_image_size.y;
		glm::vec2 rect_size(0, start_height);

		for (int tag_index = page_start_index; tag_index < page_end_index;
			 ++tag_index) {
			glm::ivec2 image_size = get_image_size(tag_indices[tag_index]);
			rect_size.x += image_size.x * start_height / image_size.y;
		}

		float scale_x = window_size.x / static_cast<float>(rect_size.x);
		float scale_y = window_size.y / static_cast<float>(rect_size.y);
		float scale = std::min(scale_x, scale_y);

		glm::ivec2 scaled_size = glm::vec2(rect_size) * scale;
		glm::ivec2 offset = (window_size - scaled_size) / 2;

		std::vector<std::pair<image_pos, glm::ivec4>> sizes_offsets;
		int running_offset = 0;
		for (int tag_index = page_end_index - 1; tag_index >= page_start_index;
			 --tag_index) {
			glm::ivec2 image_size = get_image_size(tag_indices[tag_index]);
			int scaled_width = image_size.x * scaled_size.y / image_size.y;

			sizes_offsets.emplace(
				sizes_offsets.begin(), image_pos(pos.tag, tag_index),
				glm::ivec4(scaled_width, scaled_size.y,
						   offset.x + running_offset, offset.y));

			running_offset += scaled_width;
		}

		return sizes_offsets;
	}

	std::vector<std::pair<image_pos, glm::ivec4>> get_current_render_data() {
		if (curr_image_pos.tag_index == -1)
			return {};

		std::vector<std::pair<image_pos, glm::ivec4>> sizes_offsets;

		if (curr_view_mode == view_mode::vertical) {
			image_pos pos = curr_image_pos;
			float offset_y = vertical_offset;

			while (offset_y < window_size.y) {
				int image_index = tags_indices[pos.tag][pos.tag_index];
				glm::ivec4 scaled_size_offset =
					vertical_slice_center(image_index);
				scaled_size_offset.w += offset_y;
				sizes_offsets.emplace_back(pos, scaled_size_offset);

				offset_y += scaled_size_offset.y;
				image_pos new_pos = try_advance_pos(pos, 1);
				if (new_pos == pos)
					break;
				else
					pos = new_pos;
			}
		} else if (curr_view_mode == view_mode::manga ||
				   curr_view_mode == view_mode::single)
			sizes_offsets = center_page(curr_image_pos);

		return sizes_offsets;
	}

	void clean_textures() {
		for (auto it = texture_used.begin(); it != texture_used.end();) {
			auto &[tex_key, used] = *it;
			if (!used) {
				auto texID_it = texture_IDs.find(tex_key);
				if (texID_it != texture_IDs.end()) {
					GLuint texID = texID_it->second.get_or(0);
					glDeleteTextures(1, &texID);
					texture_IDs.erase(texID_it);
				}

				it = texture_used.erase(it);
			} else {
				used = false;
				++it;
			}
		}
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
			if (i == indices.size() || get_image_type(indices[i]) == 3) {
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
				continue;
			}

			int type = get_image_type(indices[i]);
			first_alone_score -= (type == 1) && ((i - start) % 2 == 0);
			first_alone_score += (type == 1) && ((i - start) % 2 == 1);
			first_alone_score += (type == 2) && ((i - start) % 2 == 0);
			first_alone_score -= (type == 2) && ((i - start) % 2 == 1);

			if (paging_invert[indices[i]])
				invert_alone = !invert_alone;
		}

		return tag_page_starts;
	}

	void render() {
		auto render_data = get_current_render_data();
		std::vector<int> current_image_indices;
		for (auto [pos, size_offset] : render_data) {
			glBindTextureUnit(0,
							  get_texture(tags_indices[pos.tag][pos.tag_index],
										  size_offset.x));
			glProgramUniform2f(program.id(), 1, size_offset.z, size_offset.w);
			glProgramUniform2f(program.id(), 2, size_offset.x, size_offset.y);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			current_image_indices.push_back(
				tags_indices[pos.tag][pos.tag_index]);
		}

		if (!render_data.empty())
			for (auto pos : {try_advance_pos(render_data.front().first, -1),
							 try_advance_pos(render_data.back().first, 1)}) {
				for (auto [pos, size_offset] : center_page(pos)) {
					int image_index = tags_indices[pos.tag][pos.tag_index];
					preload_texture(image_index, size_offset.x);
					int tex_key = texture_key(image_index, size_offset.x);
					texture_used[tex_key] = true;
				}
			}
		clean_textures();

		if (current_image_indices != last_image_indices) {
			std::cout << "current_image=";
			for (auto index : current_image_indices)
				std::cout << image_paths[index] << '\t';
			std::cout << std::endl;
		}
		last_image_indices = current_image_indices;
	}

  public:
	image_viewer(const std::string &config_path) : loader_pool(4) {
		init_window();
		init_GLresources();
	}

	void run() {
		std::ios_base::sync_with_stdio(false);
		std::cin.tie(NULL);

		curr_view_mode = view_mode::manga;
		std::cout << "current_mode=manga" << std::endl;

		program.use();
		glBindVertexArray(null_vaoID);
		glClearColor(1.f, 0.f, 0.f, 1.f);

		double dt = 0;
		while (!glfwWindowShouldClose(window)) {
			double last_t = glfwGetTime();
			glfwPollEvents();
			handle_keys(dt);
			handle_stdin();

			glClear(GL_COLOR_BUFFER_BIT);
			render();
			glfwSwapBuffers(window);

			dt = glfwGetTime() - last_t;
		}
	}

	~image_viewer() {
		glDeleteTextures(1, &white_texID);
		for (auto &[tex_key, tex] : texture_IDs) {
			GLuint texID = tex.get_or(0);
			glDeleteTextures(1, &texID);
		}
		glDeleteVertexArrays(1, &null_vaoID);

		glfwDestroyWindow(window);
		glfwTerminate();
	}
};
