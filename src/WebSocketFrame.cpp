#include "WebSocketFrame.h"

namespace dispatcharr
{

std::vector<uint8_t> BuildMaskedControlFrame(uint8_t opcode, const std::vector<uint8_t>& payload,
                                             const uint8_t maskKey[4])
{
  std::vector<uint8_t> frame;
  frame.push_back(0x80 | opcode); // FIN + opcode
  size_t len = payload.size();
  if (len <= 125)
  {
    frame.push_back(0x80 | static_cast<uint8_t>(len)); // MASK bit set
  }
  else
  {
    frame.push_back(0x80 | 126);
    frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
    frame.push_back(static_cast<uint8_t>(len & 0xFF));
  }
  frame.insert(frame.end(), maskKey, maskKey + 4);
  for (size_t i = 0; i < len; ++i)
    frame.push_back(payload[i] ^ maskKey[i % 4]);
  return frame;
}

} // namespace dispatcharr
