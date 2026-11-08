#include "render/core/shader_program.h"

#include "render/core/startup_metrics.h"

#include <chrono>

#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
  // Prewarmed programs by their sources, until a create() takes them. The backend may prewarm on its init thread.
  std::mutex g_prewarmMutex;
  std::map<std::pair<std::string, std::string>, GLuint>& prewarmed() {
    static std::map<std::pair<std::string, std::string>, GLuint> programs;
    return programs;
  }

  GLuint compileShader(GLenum type, const char* source) {
    const GLuint shader = glCreateShader(type);
    if (shader == 0) {
      throw std::runtime_error("glCreateShader failed");
    }

    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint compiled = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (compiled == GL_FALSE) {
      GLint logLength = 0;
      glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
      std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
      glGetShaderInfoLog(shader, logLength, nullptr, log.data());
      glDeleteShader(shader);
      throw std::runtime_error("shader compilation failed: " + log);
    }

    return shader;
  }

} // namespace

ShaderProgram::~ShaderProgram() { destroy(); }

ShaderProgram::ShaderProgram(ShaderProgram&& other) noexcept : m_program(other.m_program) { other.m_program = 0; }

ShaderProgram& ShaderProgram::operator=(ShaderProgram&& other) noexcept {
  if (this == &other) {
    return *this;
  }

  destroy();
  m_program = other.m_program;
  other.m_program = 0;
  return *this;
}

void ShaderProgram::prewarm(const std::string& vertexSource, const std::string& fragmentSource) {
  std::scoped_lock lock(g_prewarmMutex);
  auto& programs = prewarmed();
  if (programs.contains({vertexSource, fragmentSource})) {
    return;
  }
  const GLuint program = glCreateProgram();
  if (program == 0) {
    return;
  }
  for (const auto& [type, source] : {std::pair{GL_VERTEX_SHADER, &vertexSource}, std::pair{GL_FRAGMENT_SHADER, &fragmentSource}}) {
    const GLuint shader = glCreateShader(type);
    const char* text = source->c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    glAttachShader(program, shader);
    glDeleteShader(shader); // freed with the program
  }
  glLinkProgram(program); // no status query here: that would wait for the job
  programs.emplace(std::pair{vertexSource, fragmentSource}, program);
}

void ShaderProgram::abandonPrewarmed() noexcept {
  std::scoped_lock lock(g_prewarmMutex);
  prewarmed().clear();
}

void ShaderProgram::create(const char* vertexSource, const char* fragmentSource) {
  destroy();
  const auto buildStart = std::chrono::steady_clock::now();
  GLuint program = 0;
  {
    std::scoped_lock lock(g_prewarmMutex);
    if (const auto it = prewarmed().find({vertexSource, fragmentSource}); it != prewarmed().end()) {
      program = it->second;
      prewarmed().erase(it);
    }
  }
  if (program != 0) {
    GLint linked = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &linked); // waits only if this job is still running
    if (linked == GL_TRUE) {
      m_program = program;
      startup_metrics::recordProgramBuild(
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count()
      );
      return;
    }
    glDeleteProgram(program); // build it again below for the compile log
  }

  const GLuint vertexShader = compileShader(GL_VERTEX_SHADER, vertexSource);
  GLuint fragmentShader = 0;
  try {
    fragmentShader = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
  } catch (...) {
    glDeleteShader(vertexShader);
    throw;
  }

  m_program = glCreateProgram();
  if (m_program == 0) {
    glDeleteShader(vertexShader);
    glDeleteShader(fragmentShader);
    throw std::runtime_error("glCreateProgram failed");
  }

  glAttachShader(m_program, vertexShader);
  glAttachShader(m_program, fragmentShader);
  glLinkProgram(m_program);

  glDeleteShader(vertexShader);
  glDeleteShader(fragmentShader);

  GLint linked = 0;
  glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
  if (linked == GL_FALSE) {
    GLint logLength = 0;
    glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLength);
    std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
    glGetProgramInfoLog(m_program, logLength, nullptr, log.data());
    destroy();
    throw std::runtime_error("shader link failed: " + log);
  }
  startup_metrics::recordProgramBuild(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - buildStart).count()
  );
}

void ShaderProgram::destroy() {
  if (m_program != 0) {
    glDeleteProgram(m_program);
    m_program = 0;
  }
}

void ShaderProgram::abandon() noexcept { m_program = 0; }

bool ShaderProgram::isValid() const noexcept { return m_program != 0; }

GLuint ShaderProgram::id() const noexcept { return m_program; }
