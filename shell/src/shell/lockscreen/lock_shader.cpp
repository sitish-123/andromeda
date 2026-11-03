#include "shell/lockscreen/lock_shader.h"

#include "render/gl_shared_context.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace {

  constexpr char kVertexShader[] = R"(
attribute vec2 a_position;
void main() {
  gl_Position = vec4(a_position * 2.0 - 1.0, 0.0, 1.0);
}
)";

  constexpr char kFragmentPrefix[] = R"(
precision highp float;
uniform vec3 iResolution;
uniform float iTime;
uniform float iTimeDelta;
uniform int iFrame;
#line 1
)";

  // Shadertoy ignores the alpha a shader writes; so does the lock.
  constexpr char kFragmentSuffix[] = R"(
void main() {
  vec4 color = vec4(0.0);
  mainImage(color, gl_FragCoord.xy);
  gl_FragColor = vec4(color.rgb, 1.0);
}
)";

  constexpr std::array<GLfloat, 12> kQuad = {0, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 1};

  GLuint compile(GLenum type, const std::string& source) {
    const GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
      std::array<char, 1024> log{};
      glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
      glDeleteShader(shader);
      throw std::runtime_error(std::string("shader compile failed: ") + log.data());
    }
    return shader;
  }

} // namespace

LockShader::LockShader(GlSharedContext& shared, const std::string& source, std::uint32_t width, std::uint32_t height)
    : m_display(shared.display()), m_width(width), m_height(height) {
  m_context = shared.createContext(shared.rootContext(), "lock shader");
  if (m_context == EGL_NO_CONTEXT
      || eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) != EGL_TRUE) {
    release();
    throw std::runtime_error("no GL context for the shader");
  }
  try {
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
        GL_UNSIGNED_BYTE, nullptr
    );
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glGenFramebuffers(1, &m_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
      throw std::runtime_error("shader framebuffer incomplete");
    }

    const GLuint vertex = compile(GL_VERTEX_SHADER, kVertexShader);
    GLuint fragment = 0;
    try {
      fragment = compile(GL_FRAGMENT_SHADER, std::string(kFragmentPrefix) + source + kFragmentSuffix);
    } catch (...) {
      glDeleteShader(vertex);
      throw;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vertex);
    glAttachShader(m_program, fragment);
    glLinkProgram(m_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
      throw std::runtime_error("shader link failed");
    }
    m_position = glGetAttribLocation(m_program, "a_position");
    m_resolution = glGetUniformLocation(m_program, "iResolution");
    m_time = glGetUniformLocation(m_program, "iTime");
    m_timeDelta = glGetUniformLocation(m_program, "iTimeDelta");
    m_frame = glGetUniformLocation(m_program, "iFrame");
  } catch (...) {
    release();
    throw;
  }
}

LockShader::~LockShader() { release(); }

bool LockShader::render(std::span<std::uint8_t> rgba) {
  const std::size_t stride = std::size_t{m_width} * 4;
  if (rgba.size() < stride * m_height
      || eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) != EGL_TRUE) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
  glViewport(0, 0, static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height));
  glDisable(GL_BLEND);
  glUseProgram(m_program);
  glUniform3f(m_resolution, static_cast<GLfloat>(m_width), static_cast<GLfloat>(m_height), 1.0F);
  glUniform1f(m_time, std::chrono::duration<float>(now - m_start).count());
  glUniform1f(m_timeDelta, std::chrono::duration<float>(now - m_last).count());
  glUniform1i(m_frame, m_frameCount++);
  m_last = now;
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray(static_cast<GLuint>(m_position));
  glVertexAttribPointer(static_cast<GLuint>(m_position), 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glDrawArrays(GL_TRIANGLES, 0, 6);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  // GL rows start at the bottom.
  std::vector<std::uint8_t> row(stride);
  for (std::size_t top = 0, bottom = m_height - 1; top < bottom; ++top, --bottom) {
    std::copy_n(rgba.data() + top * stride, stride, row.data());
    std::copy_n(rgba.data() + bottom * stride, stride, rgba.data() + top * stride);
    std::copy_n(row.data(), stride, rgba.data() + bottom * stride);
  }
  return true;
}

void LockShader::release() noexcept {
  if (m_context != EGL_NO_CONTEXT
      && eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) == EGL_TRUE) {
    if (m_program != 0) {
      glDeleteProgram(m_program);
    }
    if (m_framebuffer != 0) {
      glDeleteFramebuffers(1, &m_framebuffer);
    }
    if (m_texture != 0) {
      glDeleteTextures(1, &m_texture);
    }
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  if (m_context != EGL_NO_CONTEXT) {
    eglDestroyContext(m_display, m_context);
  }
  m_context = EGL_NO_CONTEXT;
  m_program = 0;
  m_framebuffer = 0;
  m_texture = 0;
}
