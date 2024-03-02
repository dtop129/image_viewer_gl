#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
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

//class texture_load_thread
//{
//private:
//	std::jthread worker;
//
//	std::queue<std::function<void()>> tasks;
//	std::mutex tasks_mutex;
//	std::condition_variable cv;
//
//	void worker()
//	{
//		while (true)
//		{
//
//		}
//	}
//public:
//	texture_load_thread()
//	{
//		worker = std::jthread()
//	}
//};
