"""Saved-network choice shared by the Wi-Fi screen and silent reading connect."""
from pathlib import Path
import unittest
from test_reading_ui_regressions import method, run_cpp

ROOT = Path(__file__).resolve().parents[2]


class SavedStationConnectTest(unittest.TestCase):
    def test_pick_strongest_saved_skips_attempted_and_unsaved(self):
        source = (ROOT / 'src/network/SavedStationConnect.cpp').read_text()
        body = method(source, 'const RankedSsid* pickStrongestSaved(')
        run_cpp(r'''
#include <cassert>
#include <cstddef>
#include <cstring>
struct RankedSsid { const char* ssid; int rssi; bool saved; };
''' + body + r'''
int main() {
  assert(pickStrongestSaved(nullptr, 0, nullptr, 0) == nullptr);
  const RankedSsid onlyOpen[] = {{"cafe", -30, false}};
  assert(pickStrongestSaved(onlyOpen, 1, nullptr, 0) == nullptr);

  const RankedSsid rows[] = {
    {"home", -70, true},
    {"cafe", -40, false},
    {"office", -55, true},
    {"", -20, true},
  };
  const RankedSsid* best = pickStrongestSaved(rows, 4, nullptr, 0);
  assert(best && std::strcmp(best->ssid, "office") == 0);

  const char* skip[] = {"office"};
  best = pickStrongestSaved(rows, 4, skip, 1);
  assert(best && std::strcmp(best->ssid, "home") == 0);

  const RankedSsid tied[] = {{"a", -40, true}, {"b", -40, true}};
  best = pickStrongestSaved(tied, 2, nullptr, 0);
  assert(best && std::strcmp(best->ssid, "a") == 0);
}
''')


if __name__ == '__main__':
    unittest.main()
