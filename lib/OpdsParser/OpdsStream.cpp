// Network feature: the OPDS feed stream adapter.
// Compiled out entirely on boards that answer FREEINK_CAP_NETWORK=0
// (see [base].build_flags_nonet in platformio.ini).
#if FREEINK_CAP_NETWORK

#include "OpdsStream.h"

OpdsParserStream::OpdsParserStream(OpdsParser& parser) : parser(parser) {}

int OpdsParserStream::available() { return 0; }

int OpdsParserStream::peek() { abort(); }

int OpdsParserStream::read() { abort(); }

size_t OpdsParserStream::write(uint8_t c) { return parser.write(c); }

size_t OpdsParserStream::write(const uint8_t* buffer, size_t size) { return parser.write(buffer, size); }

OpdsParserStream::~OpdsParserStream() { parser.flush(); }
#endif  // FREEINK_CAP_NETWORK
