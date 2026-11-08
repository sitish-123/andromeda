#pragma once

#include <string>

#include <GLES2/gl2.h>

class ShaderProgram {
public:
  ShaderProgram() = default;
  ~ShaderProgram();

  ShaderProgram(const ShaderProgram&) = delete;
  ShaderProgram& operator=(const ShaderProgram&) = delete;

  ShaderProgram(ShaderProgram&& other) noexcept;
  ShaderProgram& operator=(ShaderProgram&& other) noexcept;

  void create(const char* vertexSource, const char* fragmentSource);
  // Starts compiling and linking without waiting: with GL_KHR_parallel_shader_compile the driver runs every job on
  // its own threads, so a cold shader cache costs the slowest program, not the sum. A later create() with the same
  // sources takes the result. The programs belong to the current context's share group (one per process here).
  static void prewarm(const std::string& vertexSource, const std::string& fragmentSource);
  // After a GPU reset the prewarmed names belong to the dead share group: forget them without a delete.
  static void abandonPrewarmed() noexcept;
  void destroy();
  // Forget the GL name without deleting it. Used after a robust-context reset, when the
  // program is already invalid and no context is current to delete it from.
  void abandon() noexcept;

  [[nodiscard]] bool isValid() const noexcept;
  [[nodiscard]] GLuint id() const noexcept;

private:
  GLuint m_program = 0;
};
