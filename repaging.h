#pragma once

#include <vector>

std::vector<int> get_page_numbers(const std::vector<int>& indices)
{
	std::vector<int> tag_page_numbers;
	int page_index = -1;
	for (unsigned int i = 0; i < indices.size(); i++)
	{
		if (i % 2 == 0)
			page_index++;

		tag_page_numbers.push_back(page_index);
	}

	return tag_page_numbers;
}
