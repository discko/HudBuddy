#include <cassert>
#include "../firmware/hud/TapGuard.h"
int main() {
  TapGuard g;
  g.update(true,100,100); g.update(true,103,104); g.update(false,103,104);
  assert(g.allowsClick()); // Finger jitter should still allow tapping.
  g.update(true,100,100); g.update(true,100,112); g.update(false,100,112);
  assert(!g.allowsClick()); // Vertical drag, even with no scrollable content.
  g.update(true,100,100); g.update(true,100,70); g.update(true,100,100);
  g.update(false,100,100); assert(!g.allowsClick()); // Drag and return.
  g.update(true,100,100); g.update(true,130,100); g.update(false,130,100);
  assert(!g.allowsClick()); // Horizontal page gesture is not a row click.
  g.update(true,100,100); g.update(false,100,100);
  assert(g.allowsClick()); // The next genuine tap works immediately.
}
