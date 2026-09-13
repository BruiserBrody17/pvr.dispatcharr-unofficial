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

WebSocketFrameHeader ParseFrameHeaderBytes(const uint8_t header[2])
{
  WebSocketFrameHeader h;
  h.fin = (header[0] & 0x80) != 0;
  h.opcode = header[0] & 0x0F;
  h.masked = (header[1] & 0x80) != 0;
  h.payloadLength7Bit = header[1] & 0x7F;
  return h;
}

uint64_t DecodeExtendedPayloadLength16(const uint8_t ext[2])
{
  return (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
}

uint64_t DecodeExtendedPayloadLength64(const uint8_t ext[8])
{
  uint64_t len = 0;
  for (int i = 0; i < 8; ++i)
    len = (len << 8) | ext[i];
  return len;
}

void UnmaskPayload(std::vector<uint8_t>& payload, const uint8_t maskKey[4])
{
  for (size_t i = 0; i < payload.size(); ++i)
    payload[i] ^= maskKey[i % 4];
}

} // namespace dispatcharr
