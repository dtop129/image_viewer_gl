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

struct image_data {
	std::vector<uint8_t> pixels;
	glm::ivec2 size;
};

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

template <typename FutureOutput, typename FuncOutput = void> class lazy_load {
  private:
	using TransType = FuncOutput (*)(FutureOutput &&);
	using FutureType = std::future<FutureOutput>;
	using ResultType = std::conditional_t<std::is_same_v<FuncOutput, void>,
										  FutureOutput, FuncOutput>;

	FutureType future;
	ResultType result;
	TransType transform;

	bool unset = false;

  public:
	explicit lazy_load(FutureType &&fut) : future(std::move(fut)) {}

	explicit lazy_load(FutureType &&fut, TransType transfunc)
		: future(std::move(fut)), transform(transfunc) {}

	template <typename T>
	explicit lazy_load(T &&val) : result(std::forward<T>(val)) {}

	lazy_load() : unset(true) {}

	const ResultType &get() {
		if (future.valid()) {
			if constexpr (std::is_same_v<FuncOutput, void>)
				result = future.get();
			else
				result = transform(future.get());
		}
		return result;
	}

	const ResultType &get_or(const ResultType &alt) {
		if (!ready())
			return alt;
		return get();
	}

	ResultType get_or(ResultType &&alt) {
		if (!ready())
			return std::move(alt);
		return get();
	}

	bool ready() const {
		return has_value() &&
			   (!future.valid() ||
				(future.valid() && future.wait_for(std::chrono::seconds(0)) ==
									   std::future_status::ready));
	}

	bool has_value() const { return !unset; }
};

template <typename T> lazy_load(T &&val) -> lazy_load<std::remove_cvref_t<T>>;

template <typename T, typename F>
lazy_load(std::future<T> &&fut, F transfunc)
	-> lazy_load<T, std::invoke_result_t<F, T>>;

class texture_load_thread {
  private:
	std::vector<std::jthread> worker_threads;

	using req_type =
		std::tuple<std::string, glm::ivec2, std::promise<image_data>,
				   std::promise<glm::ivec2>, std::promise<int>>;

	std::deque<req_type> requests;

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

			auto [req_path, req_size, pixel_pr, size_pr, type_pr] =
				std::move(requests.back());
			requests.pop_back();
			lk.unlock();

			glm::ivec2 size;
			FILE *f = stbi__fopen(req_path.c_str(), "rb");

			bool type_set = false;
			stbi_info_from_file(f, &size.x, &size.y, nullptr);
			if (size.x > size.y * 0.8)
			{
				type_set = true;
				type_pr.set_value(3);
				if (req_size.x == 0) {
					fclose(f);
					continue;
				}
			}

			size_pr.set_value(size);
			uint8_t *pixels =
				stbi_load_from_file(f, &size.x, &size.y, nullptr, 4);

			if (!type_set)
			{
				type_pr.set_value(compute_image_type(pixels, size));
				if (req_size.x == 0) {
					stbi_image_free(pixels);
					fclose(f);
					continue;
				}
			}

			image_data data;
			data.size = req_size;
			data.pixels.resize(req_size.x * req_size.y * 4);
			resizer.resizeImage(pixels, size.x, size.y, data.pixels.data(),
								req_size.x, req_size.y, 4);

			pixel_pr.set_value(data);
			stbi_image_free(pixels);
			fclose(f);
		}
	}

  public:
	explicit texture_load_thread(unsigned int n_workers) {
		for (int i = 0; i < n_workers; ++i)
			worker_threads.emplace_back([this] { loader(); });
	}

	~texture_load_thread() {
		std::scoped_lock lk(mutex);
		stop = true;
		cv.notify_all();
	}

	auto load_texture(const std::string &path, glm::ivec2 size) {
		std::scoped_lock lk(mutex);
		auto &request = requests.emplace_back(
			path, size, std::promise<image_data>(), std::promise<glm::ivec2>(),
			std::promise<int>());
		cv.notify_one();

		return std::tuple{std::get<2>(request).get_future(),
						  std::get<3>(request).get_future(),
						  std::get<4>(request).get_future()};
	}

	auto get_image_type(const std::string &path) {
		std::scoped_lock lk(mutex);
		auto &request = requests.emplace_front(
			path, glm::ivec2(0, 0), std::promise<image_data>(),
			std::promise<glm::ivec2>(), std::promise<int>());
		cv.notify_one();

		return std::get<4>(request).get_future();
	}
};
