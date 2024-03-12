#include <GL/gl3w.h>

#include <iostream>
#include <string>

class shader_program
{
private:
	GLuint programID;

	GLuint get_shader(GLenum type, const std::string& code) {
		GLuint shader;
		const char* str = code.c_str();
		shader = glCreateShader(type);
		glShaderSource(shader, 1, &str, NULL);
		glCompileShader(shader);
		return shader;
	}

public:
	void init(const std::string& vertex_code, const std::string& fragment_code)
	{
		GLuint vertexID = get_shader(GL_VERTEX_SHADER, vertex_code);
		GLuint fragmentID = get_shader(GL_FRAGMENT_SHADER, fragment_code);

		programID = glCreateProgram();
		glAttachShader(programID, vertexID);
		glAttachShader(programID, fragmentID);
		glLinkProgram(programID);

		GLint success;
		glGetProgramiv(programID, GL_LINK_STATUS, &success);
		if (!success) {
			char log[512];
			glGetProgramInfoLog(programID, 512, NULL, log);
			std::cerr << "ERROR::SHADER::PROGRAM::LINKING_FAILED\n"
					  << log << std::endl;
			throw "shader error";
		}

		glDeleteShader(vertexID);
		glDeleteShader(fragmentID);
	}

	void destroy()
	{
		glDeleteProgram(programID);
	}

	void use() const
	{
		glUseProgram(programID);
	}

	GLuint id() const
	{
		return programID;
	}
};
