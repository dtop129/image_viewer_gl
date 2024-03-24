#pragma once

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>
#include <lancir.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define GLFW_INCLUDE_NONE
#include <GL/gl3w.h>
#include <GLFW/glfw3.h>

#include "shader.hpp"

int compute_image_type(uint8_t *pixels, glm::ivec2 size) {
	int w = size.x, h = size.y;

	// change to greyscale column major
	std::vector<uint8_t> pixels_cm;
	pixels_cm.reserve(w * h);
	for (int x = 0; x < w; x++) {
		for (int y = 0; y < h; y++) {
			const uint8_t *pixel = pixels + (x + w * y) * 4;
			pixels_cm.push_back((pixel[0] + pixel[1] + pixel[2]) / 3);
		}
	}
	int depth = 0;
	int page_type = 3;

	unsigned int var_left = 0, var_right = 0;

	while (page_type == 3 && depth++ < 20) {
		unsigned int color_left = 0, color_right = 0;
		color_left = std::accumulate(pixels_cm.begin() + h * depth,
									 pixels_cm.begin() + h * (depth + 1), 0);
		color_right = std::accumulate(pixels_cm.end() - h * (depth + 1),
									  pixels_cm.end() - h * depth, 0);

		color_left = color_left / h;
		color_right = color_right / h;

		unsigned int accum = 0;
		std::for_each(pixels_cm.begin(), pixels_cm.begin() + h,
					  [&](unsigned int d) {
						  accum += (d - color_left) * (d - color_left);
					  });
		var_left = std::max(var_left, accum / h);

		accum = 0;
		std::for_each(pixels_cm.end() - h, pixels_cm.end(),
					  [&](unsigned int d) {
						  accum += (d - color_right) * (d - color_right);
					  });
		var_right = std::max(var_right, accum / h);

		page_type = ((var_right < 500) << 1) | (var_left < 500);
	}

	if (page_type == 3)
		page_type = 0;
	return page_type;
}

template <typename T> class lazy_load {
  private:
	std::future<T> future;
	T result;

	bool unset = false;

  public:
	lazy_load(std::future<T> &&fut) : future(std::move(fut)) {}
	lazy_load() : unset(true) {}

	const T &get() {
		if (future.valid())
			result = future.get();
		return result;
	}

	const T &get_or(const T &alt) {
		if (!ready())
			return alt;
		return get();
	}

	bool ready() const {
		return !unset &&
			   (!future.valid() ||
				(future.valid() && future.wait_for(std::chrono::seconds(0)) ==
									   std::future_status::ready));
	}

	bool has_value() const { return !unset; }
};

class texture_load_pool {
  private:
	std::vector<std::jthread> worker_threads;
	GLFWwindow *load_window;
	shader_program program;
	GLuint nullVAO;

	using req_type = std::tuple<std::string, glm::ivec2, std::promise<GLuint>,
								std::promise<glm::ivec2>, std::promise<int>>;

	std::deque<req_type> requests;

	std::mutex context_mutex;
	std::mutex mutex;
	std::condition_variable cv;
	bool stop = false;

	void loader() {
		avir::CLancIR resizer;

		while (true) {
			std::unique_lock lk(mutex);
			if (requests.empty())
				cv.wait(lk, [this] { return !requests.empty() || stop; });
			if (stop)
				return;

			auto [req_path, req_size, texture_pr, size_pr, type_pr] =
				std::move(requests.back());
			requests.pop_back();
			lk.unlock();

			glm::ivec2 size;
			if (req_size.x == 0) {
				FILE *f = stbi__fopen(req_path.c_str(), "rb");
				stbi_info_from_file(f, &size.x, &size.y, nullptr);
				size_pr.set_value(size);

				if (size.x > size.y * 0.8)
					type_pr.set_value(3);
				else {
					uint8_t *pixels =
						stbi_load_from_file(f, &size.x, &size.y, nullptr, 4);
					type_pr.set_value(compute_image_type(pixels, size));
					stbi_image_free(pixels);
				}
				fclose(f);
			} else {
				uint8_t *pixels =
					stbi_load(req_path.c_str(), &size.x, &size.y, nullptr, 4);
				std::vector<uint8_t> resized_pixels(req_size.x * req_size.y *
													4);
				resizer.resizeImage(pixels, size.x, size.y,
									resized_pixels.data(), req_size.x,
									req_size.y, 4);

				std::scoped_lock lk(context_mutex);
				glfwMakeContextCurrent(load_window);
				GLuint tex;
				glCreateTextures(GL_TEXTURE_2D, 1, &tex);
				glTextureStorage2D(tex, 1, GL_RGBA8, req_size.x, req_size.y);
				glTextureSubImage2D(tex, 0, 0, 0, req_size.x, req_size.y,
									GL_RGBA, GL_UNSIGNED_BYTE,
									resized_pixels.data());
				glBindTextureUnit(0, tex);
				glDrawArrays(GL_TRIANGLES, 0, 3);
				glFinish();
				texture_pr.set_value(tex);
				glfwMakeContextCurrent(nullptr);
				stbi_image_free(pixels);
			}
		}
	}

  public:
	void init(GLFWwindow *load_window, unsigned int n_workers) {
		this->load_window = load_window;
		for (int i = 0; i < n_workers; ++i)
			worker_threads.emplace_back([this] { loader(); });

		const std::string vert_shader = R"(
#version 460 core
out vec2 fs_texcoords;
void main()
{
	const vec2 pos_arr[3] = {{-1, -1}, {3, -1}, {1, -3}};
	gl_Position = vec4(pos_arr[gl_VertexID], 0, 1);
	fs_texcoords = 0.5 * gl_Position.xy + vec2(0.5);
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
		GLFWwindow *prev_context = glfwGetCurrentContext();
		glfwMakeContextCurrent(load_window);
		glCreateVertexArrays(1, &nullVAO);
		glBindVertexArray(nullVAO);
		program.init(vert_shader, frag_shader);
		program.use();
		glfwMakeContextCurrent(prev_context);
	}

	void destroy() {
		GLFWwindow *prev_context = glfwGetCurrentContext();
		glfwMakeContextCurrent(load_window);
		glDeleteVertexArrays(1, &nullVAO);
		program.destroy();
		glfwMakeContextCurrent(prev_context);

		std::scoped_lock lk(mutex);
		stop = true;
		cv.notify_all();
	}

	auto load_texture(const std::string &path, glm::ivec2 size) {
		std::scoped_lock lk(mutex);
		auto &request = requests.emplace_back(
			path, size, std::promise<GLuint>(), std::promise<glm::ivec2>(),
			std::promise<int>());
		cv.notify_one();

		return std::get<2>(request).get_future();
	}

	auto get_size_type(const std::string &path) {
		std::scoped_lock lk(mutex);
		auto &request = requests.emplace_front(
			path, glm::ivec2(0, 0), std::promise<GLuint>(),
			std::promise<glm::ivec2>(), std::promise<int>());
		cv.notify_one();

		return std::pair{std::get<3>(request).get_future(),
						 std::get<4>(request).get_future()};
	}
};
