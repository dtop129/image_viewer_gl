#include <GL/gl3w.h>

#include <fstream>
#include <iostream>
#include <string>

class shader_program
{
private:
	GLuint programID;

	GLuint get_shader(GLenum type, const std::string& path) {
		GLuint shader;
		shader = glCreateShader(type);

		std::ifstream ifs(path);
		std::string content((std::istreambuf_iterator<char>(ifs)),
							(std::istreambuf_iterator<char>()));
		const char* str = content.c_str();
		glShaderSource(shader, 1, &str, NULL);
		glCompileShader(shader);
		return shader;
	}

public:
	void init(const std::string& vertex_path, const std::string& fragment_path)
	{
		GLuint vertexID = get_shader(GL_VERTEX_SHADER, vertex_path);
		GLuint fragmentID = get_shader(GL_FRAGMENT_SHADER, fragment_path);

		programID = glCreateProgram();
		glAttachShader(programID, vertexID);
		glAttachShader(programID, fragmentID);
		glLinkProgram(programID);

		GLint success;
		glGetProgramiv(programID, GL_LINK_STATUS, &success);
		if (!success) {
			char log[512];
			glGetProgramInfoLog(programID, 512, NULL, log);
			std::cout << "ERROR::SHADER::PROGRAM::LINKING_FAILED\n"
					  << log << std::endl;
			throw "shader error";
		}

		glDeleteShader(vertexID);
		glDeleteShader(fragmentID);
	}

	void use() const
	{
		glUseProgram(programID);
	}

	GLuint id() const
	{
		return programID;
	}

	~shader_program()
	{
		glDeleteProgram(programID);
	}
};
