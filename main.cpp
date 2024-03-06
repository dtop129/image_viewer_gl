#include <string>

#include "app.hpp"

int main(int argc, char **argv) {
	std::string config_path;

	for (int i = 1; i < argc; i++) {
		if (std::string(argv[i]) == "--config")
			config_path = argv[++i];
	}

	image_viewer app(config_path);
	app.run();
	return 0;
}
