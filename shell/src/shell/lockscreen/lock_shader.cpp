#include "shell/lockscreen/lock_shader.h"

#include "render/gl_shared_context.h"

#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <cstdint>
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
uniform sampler2D iChannel0;
uniform vec3 iChannelResolution[4];
#line 1
)";

  // Shadertoy ignores the alpha a shader writes; so does the lock. fragCoord keeps Shadertoy's
  // bottom-left origin while row 0 of the buffer holds the top row, as the shell reads images.
  constexpr char kFragmentSuffix[] = R"(
void main() {
  vec4 color = vec4(0.0);
  mainImage(color, vec2(gl_FragCoord.x, iResolution.y - gl_FragCoord.y));
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
    glGenTextures(2, m_textures.data());
    glGenFramebuffers(2, m_framebuffers.data());
    glGenTextures(1, &m_channel);
    glBindTexture(GL_TEXTURE_2D, m_channel);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    for (std::size_t i = 0; i < 2; ++i) {
      glBindTexture(GL_TEXTURE_2D, m_textures[i]);
      glTexImage2D(
          GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
          GL_UNSIGNED_BYTE, nullptr
      );
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffers[i]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_textures[i], 0);
      if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("shader framebuffer incomplete");
      }
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
    m_channel0 = glGetUniformLocation(m_program, "iChannel0");
    m_channelResolution = glGetUniformLocation(m_program, "iChannelResolution");
    exportDmabufs();
  } catch (...) {
    release();
    throw;
  }
}

LockShader::~LockShader() { release(); }

bool LockShader::makeCurrent() {
  return eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) == EGL_TRUE;
}

void LockShader::exportDmabufs() {
  const auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  const auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  const auto query = reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC>(
      eglGetProcAddress("eglExportDMABUFImageQueryMESA")
  );
  const auto exportImage =
      reinterpret_cast<PFNEGLEXPORTDMABUFIMAGEMESAPROC>(eglGetProcAddress("eglExportDMABUFImageMESA"));
  if (createImage == nullptr || destroyImage == nullptr || query == nullptr || exportImage == nullptr) {
    return;
  }
  for (const GLuint texture : m_textures) {
    const EGLImageKHR image = createImage(
        m_display, m_context, EGL_GL_TEXTURE_2D_KHR, reinterpret_cast<EGLClientBuffer>(std::uintptr_t{texture}),
        nullptr
    );
    int fourcc = 0;
    int planes = 0;
    EGLuint64KHR modifier = 0;
    int fd = -1;
    EGLint stride = 0;
    EGLint offset = 0;
    const bool exported = image != EGL_NO_IMAGE_KHR
        && query(m_display, image, &fourcc, &planes, &modifier) == EGL_TRUE
        && planes == 1
        && exportImage(m_display, image, &fd, &stride, &offset) == EGL_TRUE;
    if (image != EGL_NO_IMAGE_KHR) {
      destroyImage(m_display, image);
    }
    if (!exported) {
      // Both buffers or none: the memfd path covers the rest.
      for (const auto& dmabuf : m_dmabufs) {
        ::close(dmabuf.fd);
      }
      m_dmabufs.clear();
      return;
    }
    m_dmabufs.push_back({
        .fd = fd,
        .width = m_width,
        .height = m_height,
        .fourcc = static_cast<std::uint32_t>(fourcc),
        .modifier = modifier,
        .stride = static_cast<std::uint32_t>(stride),
        .offset = static_cast<std::uint32_t>(offset),
    });
  }
}

bool LockShader::setChannel0Dmabuf(const PixelStreamServer::Dmabuf& buffer) {
  const auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
  const auto targetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
      eglGetProcAddress("glEGLImageTargetTexture2DOES")
  );
  if (createImage == nullptr || targetTexture == nullptr || !makeCurrent()) {
    return false;
  }
  const std::array<EGLint, 17> attributes = {
      EGL_WIDTH,
      static_cast<EGLint>(buffer.width),
      EGL_HEIGHT,
      static_cast<EGLint>(buffer.height),
      EGL_LINUX_DRM_FOURCC_EXT,
      static_cast<EGLint>(buffer.fourcc),
      EGL_DMA_BUF_PLANE0_FD_EXT,
      buffer.fd,
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      static_cast<EGLint>(buffer.offset),
      EGL_DMA_BUF_PLANE0_PITCH_EXT,
      static_cast<EGLint>(buffer.stride),
      EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
      static_cast<EGLint>(buffer.modifier & 0xffffffffU),
      EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
      static_cast<EGLint>(buffer.modifier >> 32U),
      EGL_NONE,
  };
  while (glGetError() != GL_NO_ERROR) {
  }
  const EGLImageKHR image = createImage(m_display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attributes.data());
  if (image == EGL_NO_IMAGE_KHR) {
    return false;
  }
  glBindTexture(GL_TEXTURE_2D, m_channel);
  targetTexture(GL_TEXTURE_2D, image);
  releaseChannelImage();
  m_channelImage = image;
  m_channelWidth = buffer.width;
  m_channelHeight = buffer.height;
  return glGetError() == GL_NO_ERROR;
}

bool LockShader::setChannel0Pixels(std::span<const std::uint8_t> rgba, std::uint32_t width, std::uint32_t height) {
  if (rgba.size() < std::size_t{width} * 4 * height || !makeCurrent()) {
    return false;
  }
  while (glGetError() != GL_NO_ERROR) {
  }
  releaseChannelImage();
  glBindTexture(GL_TEXTURE_2D, m_channel);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  if (width == m_channelWidth && height == m_channelHeight) {
    glTexSubImage2D(
        GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
        rgba.data()
    );
  } else {
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA, static_cast<GLsizei>(width), static_cast<GLsizei>(height), 0, GL_RGBA,
        GL_UNSIGNED_BYTE, rgba.data()
    );
  }
  m_channelWidth = width;
  m_channelHeight = height;
  return glGetError() == GL_NO_ERROR;
}

void LockShader::releaseChannelImage() noexcept {
  if (m_channelImage == EGL_NO_IMAGE) {
    return;
  }
  const auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
  if (destroyImage != nullptr) {
    destroyImage(m_display, m_channelImage);
  }
  m_channelImage = EGL_NO_IMAGE;
  // A texture that held an EGLImage gets fresh storage on the next upload.
  m_channelWidth = 0;
  m_channelHeight = 0;
}

bool LockShader::draw(std::size_t index) {
  if (index >= m_framebuffers.size() || !makeCurrent()) {
    return false;
  }
  const auto now = std::chrono::steady_clock::now();
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffers[index]);
  glViewport(0, 0, static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height));
  glDisable(GL_BLEND);
  glUseProgram(m_program);
  glUniform3f(m_resolution, static_cast<GLfloat>(m_width), static_cast<GLfloat>(m_height), 1.0F);
  glUniform1f(m_time, std::chrono::duration<float>(now - m_start).count());
  glUniform1f(m_timeDelta, std::chrono::duration<float>(now - m_last).count());
  glUniform1i(m_frame, m_frameCount++);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_channel);
  glUniform1i(m_channel0, 0);
  glUniform3f(m_channelResolution, static_cast<GLfloat>(m_channelWidth), static_cast<GLfloat>(m_channelHeight), 1.0F);
  m_last = now;
  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glEnableVertexAttribArray(static_cast<GLuint>(m_position));
  glVertexAttribPointer(static_cast<GLuint>(m_position), 2, GL_FLOAT, GL_FALSE, 0, kQuad.data());
  glDrawArrays(GL_TRIANGLES, 0, 6);
  // The shell samples the dmabuf as soon as FRAME arrives; it has no fence to wait on.
  glFinish();
  return true;
}

bool LockShader::read(std::size_t index, std::span<std::uint8_t> rgba) {
  if (index >= m_framebuffers.size() || rgba.size() < std::size_t{m_width} * 4 * m_height || !makeCurrent()) {
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffers[index]);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, static_cast<GLsizei>(m_width), static_cast<GLsizei>(m_height), GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
  return true;
}

void LockShader::release() noexcept {
  if (m_context != EGL_NO_CONTEXT
      && eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_context) == EGL_TRUE) {
    if (m_program != 0) {
      glDeleteProgram(m_program);
    }
    glDeleteFramebuffers(2, m_framebuffers.data());
    glDeleteTextures(2, m_textures.data());
    glDeleteTextures(1, &m_channel);
    eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  }
  releaseChannelImage();
  if (m_context != EGL_NO_CONTEXT) {
    eglDestroyContext(m_display, m_context);
  }
  for (const auto& dmabuf : m_dmabufs) {
    ::close(dmabuf.fd);
  }
  m_dmabufs.clear();
  m_context = EGL_NO_CONTEXT;
  m_program = 0;
  m_channel = 0;
  m_framebuffers = {};
  m_textures = {};
}
