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

int compute_image_type(uint8_t *pixels, int w, int h) {
	if (w > 0.8 * h)
		return 3;

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

	ResultType get() {
		if (future.valid()) {
			if constexpr (std::is_same_v<FuncOutput, void>)
				result = future.get();
			else
				result = transform(future.get());
		}
		return result;
	}

	ResultType get_or(const ResultType &alt) {
		if (!ready())
			return alt;
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

	using req_type = std::tuple<std::string, int, std::promise<image_data>,
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

			auto [req_path, req_width, pixel_pr, size_pr, type_pr] =
				std::move(requests.back());
			requests.pop_back();
			lk.unlock();

			int width, height;
			if (req_width == -1) {
				stbi_info(req_path.c_str(), &width, &height, nullptr);
				size_pr.set_value({width, height});
				continue;
			}

			uint8_t *pixels =
				stbi_load(req_path.c_str(), &width, &height, nullptr, 4);

			if (req_width == -2)
				type_pr.set_value(compute_image_type(pixels, width, height));
			else {
				image_data data;
				int req_height = height * req_width / width;
				data.size = {req_width, req_height};
				data.pixels.resize(req_width * req_height * 4);
				resizer.resizeImage(pixels, width, height, data.pixels.data(),
									req_width, req_height, 4);

				pixel_pr.set_value(data);
			}

			stbi_image_free(pixels);
		}
	}

	template <int n_pr> auto add_request(const std::string &path, int width) {
		req_type request;
		std::get<0>(request) = path;
		std::get<1>(request) = width;
		auto future = std::get<2 + n_pr>(request).get_future();
		{
			std::scoped_lock lock(mutex);
			if (width < 0)
				requests.push_front(std::move(request));
			else
				requests.push_back(std::move(request));
		}

		cv.notify_one();
		return future;
	}

  public:
	explicit texture_load_thread(unsigned int n_workers) {
		for (int i = 0; i < n_workers; ++i)
			worker_threads.emplace_back([this] { loader(); });
	}

	~texture_load_thread() {
		{
			std::scoped_lock lk(mutex);
			stop = true;
		}
		cv.notify_all();
	}

	std::future<image_data> load_texture(const std::string &path, int width) {
		return add_request<0>(path, width);
	}
	std::future<glm::ivec2> get_image_size(const std::string &path) {
		return add_request<1>(path, -1);
	}
	std::future<int> get_image_type(const std::string &path) {
		return add_request<2>(path, -2);
	}
};
