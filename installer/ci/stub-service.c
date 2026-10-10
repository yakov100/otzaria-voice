/* Stand-in for the real service, used only by the workflow's manual
 * "stub_service" run to exercise the Linux/macOS packaging before the
 * portable service sources exist. It is never part of a real build. */
#include <stdio.h>

int main(int argc, char** argv) {
  printf("otzaria-voice packaging stub (%d args%s%s)\n", argc - 1, argc > 1 ? ": " : "",
         argc > 1 ? argv[1] : "");
  return 0;
}
