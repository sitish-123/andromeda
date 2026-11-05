#include "../src/helper_client.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h>

namespace {
  std::string writeTemp(const std::string& name, const std::string& body) {
    const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/" + name + "-"
        + std::to_string(getpid());
    std::ofstream(path) << body;
    return path;
  }
} // namespace

int main() {
  using namespace preview;
  int failures = 0;

  const std::string textPath = writeTemp("preview-text", "line one\nline two\n");
  auto text = HelperClient::start(HelperKind::Text, textPath);
  std::string got;
  bool answered = false;
  if (text) {
    text->request({.op = op::Text}, [&](const HelperReply& reply) {
      answered = true;
      if (reply.msg.op == op::TextReply && reply.data) {
        got.assign(reinterpret_cast<const char*>(reply.data->bytes().data()), reply.msg.a);
      }
    });
    text->drain();
  }
  if (!answered || got != "line one\nline two\n") {
    std::fprintf(stderr, "FAIL text reply: answered=%d got='%s'\n", answered, got.c_str());
    ++failures;
  }

  // A file poppler rejects: the helper exits, and the pending request gets the failure reply.
  const std::string pdfPath = writeTemp("preview-bad", "%PDF-1.7\nbroken");
  auto pdf = HelperClient::start(HelperKind::Pdf, pdfPath);
  bool failed = false;
  if (pdf) {
    pdf->request({.op = op::Info}, [&](const HelperReply& reply) { failed = !reply.ok(); });
    pdf->drain();
  }
  if (!failed || (pdf && pdf->alive())) {
    std::fprintf(stderr, "FAIL broken pdf: failed=%d\n", failed);
    ++failures;
  }

  unlink(textPath.c_str());
  unlink(pdfPath.c_str());
  return failures == 0 ? 0 : 1;
}
