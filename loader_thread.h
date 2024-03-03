#pragma once

#include <condition_variable>
#include <future>
#include <mutex>
#include <stack>
#include <string>
#include <thread>
#include <vector>

#include <glm/glm.hpp>
#include <lancir.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

struct image_data
{
	std::vector<uint8_t> pixels;
	glm::ivec2 size;
};

class texture_load_thread
{
private:
	std::vector<std::jthread> worker_threads;

	std::stack<std::tuple<std::string, int, std::promise<image_data>>> requests;

	std::mutex mutex;
	std::condition_variable cv;
	bool stop = false;

	void loader()
	{
		avir::CLancIR resizer;

		while (true)
		{
			std::unique_lock lk(mutex);
			if (requests.empty())
				cv.wait(lk, [this]{ return !requests.empty() || stop; });
			if (stop)
				return;

			auto[req_path, req_width, promise] = std::move(requests.top());
			requests.pop();
			lk.unlock();

			int width, height;
			uint8_t* pixels = stbi_load(req_path.c_str(), &width, &height, nullptr, 4);
			int req_height = height * req_width / width;

			image_data data;
			data.size = {req_width, req_height};
			data.pixels.resize(req_width * req_height * 4);
			resizer.resizeImage(pixels, width, height, data.pixels.data(), req_width, req_height, 4);

			promise.set_value(std::move(data));
			stbi_image_free(pixels);
		}
	}
public:
	texture_load_thread(unsigned int n_workers)
	{
		for (int i = 0; i < n_workers; ++i)
			worker_threads.emplace_back([this]{ loader(); });
	}

	~texture_load_thread()
	{
		{
			std::scoped_lock lk(mutex);
			stop = true;
		}
		cv.notify_all();
	}

	std::future<image_data> load_texture(const std::string& path, int width)
	{
		auto request = std::tuple(path, width, std::promise<image_data>());
		auto future = std::get<2>(request).get_future();
		{
			std::scoped_lock lock(mutex);
			requests.emplace(std::move(request));
		}

		cv.notify_one();
		return future;
	}
};
