// Protocol Client for the MinimalRTDE rig. See plantForCppRTDE.md.
// The URScript must already be playing. This process speaks RTDE itself.

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr int kRtdePort = 30004;
constexpr double kFrequencyHz = 250.0;
constexpr int kConnectTimeoutMs = 5000;
constexpr int kIoTimeoutSec = 2;
constexpr uint32_t kPlaying = 2;

constexpr uint8_t kRequestProtocolVersion = 86;  // 'V'
constexpr uint8_t kTextMessage = 77;             // 'M'
constexpr uint8_t kDataPackage = 85;             // 'U'
constexpr uint8_t kSetupOutputs = 79;            // 'O'
constexpr uint8_t kSetupInputs = 73;             // 'I'
constexpr uint8_t kStart = 83;                   // 'S'
constexpr uint8_t kPause = 80;                   // 'P'

constexpr char kOutputNames[] =
    "timestamp,runtime_state,output_int_register_41,output_int_register_42";
constexpr char kInputNames[] = "input_int_register_42";
constexpr char kOutputTypes[] = "DOUBLE,UINT32,INT32,INT32";
constexpr char kInputTypes[] = "INT32";

enum class Stop { Duration, RuntimeState, Protocol };

struct Row {
  double robot_timestamp = 0.0;
  int32_t client_counter = 0;
  int32_t echo_counter = 0;
  int32_t script_counter = 0;
  uint32_t runtime_state = 0;
};

struct Args {
  std::string host = "192.168.178.56";
  double duration_s = 60.0;
  std::string output = "protocol_client.csv";
};

struct Run {
  Stop stop = Stop::Protocol;
  std::string detail;
  std::vector<Row> rows;
  bool socket_open = false;
};

class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
  Socket& operator=(Socket&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = other.fd_;
      other.fd_ = -1;
    }
    return *this;
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket() { close(); }

  int fd() const { return fd_; }
  bool open() const { return fd_ >= 0; }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

bool little_endian() {
  const uint16_t probe = 1;
  uint8_t first = 0;
  std::memcpy(&first, &probe, 1);
  return first == 1;
}

void append_bytes(std::vector<uint8_t>& out, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  if (little_endian()) {
    out.insert(out.end(), bytes, bytes + size);
    std::reverse(out.end() - static_cast<std::ptrdiff_t>(size), out.end());
  } else {
    out.insert(out.end(), bytes, bytes + size);
  }
}

void append_u16(std::vector<uint8_t>& out, uint16_t value) { append_bytes(out, &value, sizeof(value)); }
void append_i32(std::vector<uint8_t>& out, int32_t value) { append_bytes(out, &value, sizeof(value)); }
void append_f64(std::vector<uint8_t>& out, double value) { append_bytes(out, &value, sizeof(value)); }

template <typename T>
T read_be(const uint8_t* data) {
  T value{};
  std::memcpy(&value, data, sizeof(value));
  if (little_endian()) {
    auto* bytes = reinterpret_cast<uint8_t*>(&value);
    std::reverse(bytes, bytes + sizeof(value));
  }
  return value;
}

std::vector<uint8_t> make_packet(uint8_t type, const std::vector<uint8_t>& payload) {
  const auto size = static_cast<uint16_t>(3 + payload.size());
  std::vector<uint8_t> packet;
  packet.reserve(size);
  append_u16(packet, size);
  packet.push_back(type);
  packet.insert(packet.end(), payload.begin(), payload.end());
  return packet;
}

void send_all(int fd, const std::vector<uint8_t>& packet) {
  std::size_t sent = 0;
  while (sent < packet.size()) {
    const ssize_t n = ::send(fd, packet.data() + sent, packet.size() - sent, MSG_NOSIGNAL);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw std::runtime_error(std::string("send failed: ") + std::strerror(errno));
    }
    if (n == 0) {
      throw std::runtime_error("send wrote no bytes");
    }
    sent += static_cast<std::size_t>(n);
  }
}

void send_command(int fd, uint8_t type, const std::vector<uint8_t>& payload = {}) {
  send_all(fd, make_packet(type, payload));
}

class Reader {
 public:
  explicit Reader(int fd) : fd_(fd) {}

  bool next(uint8_t& type, std::vector<uint8_t>& payload, std::string& error) {
    if (!ensure(3, error)) {
      return false;
    }
    const uint16_t size = read_be<uint16_t>(buffer_.data());
    if (size < 3) {
      error = "package size is smaller than the header";
      return false;
    }
    if (size > 2048) {
      error = "package size " + std::to_string(size) + " is larger than this client accepts";
      return false;
    }
    if (!ensure(size, error)) {
      return false;
    }
    type = buffer_[2];
    payload.assign(buffer_.begin() + 3, buffer_.begin() + size);
    buffer_.erase(buffer_.begin(), buffer_.begin() + size);
    return true;
  }

 private:
  bool ensure(std::size_t need, std::string& error) {
    while (buffer_.size() < need) {
      uint8_t chunk[4096];
      const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
      if (n < 0) {
        if (errno == EINTR) {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          error = "no package arrived within " + std::to_string(kIoTimeoutSec) + " s";
          return false;
        }
        error = std::string("read failed: ") + std::strerror(errno);
        return false;
      }
      if (n == 0) {
        error = "connection closed";
        return false;
      }
      buffer_.insert(buffer_.end(), chunk, chunk + n);
    }
    return true;
  }

  int fd_ = -1;
  std::vector<uint8_t> buffer_;
};

std::string text_message(const std::vector<uint8_t>& payload) {
  if (payload.size() < 2) {
    return "controller text message was truncated";
  }
  std::size_t offset = 0;
  const uint8_t message_length = payload[offset++];
  if (offset + message_length + 1 > payload.size()) {
    return "controller text message was truncated";
  }
  const std::string message(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                            payload.begin() + static_cast<std::ptrdiff_t>(offset + message_length));
  offset += message_length;
  const uint8_t source_length = payload[offset++];
  if (offset + source_length > payload.size()) {
    return message;
  }
  const std::string source(payload.begin() + static_cast<std::ptrdiff_t>(offset),
                           payload.begin() + static_cast<std::ptrdiff_t>(offset + source_length));
  if (source.empty()) {
    return message;
  }
  return source + ": " + message;
}

bool take_packet(Reader& reader, uint8_t expected, std::vector<uint8_t>& payload, std::string& error) {
  uint8_t type = 0;
  if (!reader.next(type, payload, error)) {
    return false;
  }
  if (type == kTextMessage) {
    error = "controller text message: " + text_message(payload);
    return false;
  }
  if (type != expected) {
    error = "expected package " + std::to_string(expected) + ", received " + std::to_string(type);
    return false;
  }
  return true;
}

std::vector<std::string> split_comma(const std::string& text) {
  std::vector<std::string> parts;
  std::stringstream stream(text);
  std::string part;
  while (std::getline(stream, part, ',')) {
    parts.push_back(part);
  }
  return parts;
}

std::string recipe_types(const std::vector<uint8_t>& payload, uint8_t& recipe_id) {
  if (payload.empty()) {
    throw std::runtime_error("recipe answer had no payload");
  }
  recipe_id = payload[0];
  return std::string(payload.begin() + 1, payload.end());
}

void require_recipe(const std::string& kind, uint8_t recipe_id, const std::string& actual,
                    const std::string& expected) {
  const std::vector<std::string> types = split_comma(actual);
  for (const std::string& type : types) {
    if (type == "NOT_FOUND" || type == "IN_USE") {
      throw std::runtime_error(kind + " recipe was rejected (" + actual + ")");
    }
  }
  if (recipe_id == 0 || actual != expected) {
    throw std::runtime_error(kind + " recipe types were " + actual + ", expected " + expected);
  }
}

void usage() {
  std::cout
      << "protocol_client [--host IP] [--duration SEC] [--output PATH]\n"
      << "\n"
      << "Exchange the Client Counter at 250 Hz over RTDE port " << kRtdePort << ".\n"
      << "counter_exchange.urscript must already be playing on the robot.\n"
      << "No other RTDE writer may own input_int_register_42.\n"
      << "\n"
      << "  --host       robot address (default 192.168.178.56)\n"
      << "  --duration   seconds to exchange (default 60)\n"
      << "  --output     CSV path (default protocol_client.csv)\n";
}

Args parse_args(int argc, char** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--help" || flag == "-h") {
      usage();
      std::exit(0);
    }
    if (i + 1 >= argc) {
      throw std::runtime_error("missing value for " + flag);
    }
    const std::string value = argv[++i];
    if (flag == "--host") {
      args.host = value;
    } else if (flag == "--duration") {
      try {
        args.duration_s = std::stod(value);
      } catch (const std::exception&) {
        throw std::runtime_error("duration must be a number of seconds");
      }
      if (!(args.duration_s > 0.0)) {
        throw std::runtime_error("duration must be greater than 0");
      }
    } else if (flag == "--output") {
      args.output = value;
    } else {
      throw std::runtime_error("unknown argument " + flag);
    }
  }
  if (args.host.empty() || args.output.empty()) {
    throw std::runtime_error("host and output must not be empty");
  }
  return args;
}

void set_timeouts(int fd) {
  timeval limit{};
  limit.tv_sec = kIoTimeoutSec;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit)) != 0 ||
      setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit)) != 0) {
    throw std::runtime_error(std::string("could not set socket timeouts: ") + std::strerror(errno));
  }
}

Socket connect_robot(const std::string& host) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const int lookup = getaddrinfo(host.c_str(), std::to_string(kRtdePort).c_str(), &hints, &result);
  if (lookup != 0) {
    throw std::runtime_error(std::string("could not resolve ") + host + ": " + gai_strerror(lookup));
  }

  std::string last_error = "no address to connect to";
  for (addrinfo* info = result; info != nullptr; info = info->ai_next) {
    const int fd = ::socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (fd < 0) {
      last_error = std::strerror(errno);
      continue;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
      last_error = std::strerror(errno);
      ::close(fd);
      continue;
    }
    const int rc = ::connect(fd, info->ai_addr, info->ai_addrlen);
    if (rc != 0 && errno != EINPROGRESS) {
      last_error = std::strerror(errno);
      ::close(fd);
      continue;
    }
    pollfd watch{};
    watch.fd = fd;
    watch.events = POLLOUT;
    int polled = -1;
    do {
      polled = ::poll(&watch, 1, kConnectTimeoutMs);
    } while (polled < 0 && errno == EINTR);
    if (polled == 0) {
      last_error = "timed out connecting to " + host;
      ::close(fd);
      continue;
    }
    if (polled < 0) {
      last_error = std::strerror(errno);
      ::close(fd);
      continue;
    }
    int so_error = 0;
    socklen_t length = sizeof(so_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) != 0 || so_error != 0) {
      last_error = so_error != 0 ? std::strerror(so_error) : std::strerror(errno);
      ::close(fd);
      continue;
    }
    if (fcntl(fd, F_SETFL, flags) != 0) {
      last_error = std::strerror(errno);
      ::close(fd);
      continue;
    }
    const int enabled = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
      last_error = std::string("could not enable TCP_NODELAY: ") + std::strerror(errno);
      ::close(fd);
      continue;
    }
    Socket connected(fd);
    try {
      set_timeouts(connected.fd());
    } catch (...) {
      freeaddrinfo(result);
      throw;
    }
    freeaddrinfo(result);
    return connected;
  }
  freeaddrinfo(result);
  throw std::runtime_error("could not connect to " + host + ": " + last_error);
}

const char* policy_name(int policy) {
  switch (policy) {
    case SCHED_FIFO:
      return "SCHED_FIFO";
    case SCHED_RR:
      return "SCHED_RR";
    case SCHED_OTHER:
      return "SCHED_OTHER";
    default:
      return "unknown";
  }
}

void raise_priority() {
  const int maximum = sched_get_priority_max(SCHED_FIFO);
  const int minimum = sched_get_priority_min(SCHED_FIFO);
  bool granted = false;
  if (maximum >= minimum) {
    for (int priority = maximum; priority >= minimum; --priority) {
      sched_param parameters{};
      parameters.sched_priority = priority;
      if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &parameters) == 0) {
        granted = true;
        break;
      }
    }
  }

  int policy = SCHED_OTHER;
  sched_param actual{};
  const bool readable = pthread_getschedparam(pthread_self(), &policy, &actual) == 0;
  if (!readable) {
    std::cout << "Could not read the scheduling priority in effect." << std::endl;
    std::cout << "The highest priority was not granted." << std::endl;
    return;
  }

  std::cout << "Scheduling policy " << policy_name(policy) << ", priority " << actual.sched_priority << "."
            << std::endl;
  const bool highest = granted && policy == SCHED_FIFO && actual.sched_priority == maximum;
  if (!highest) {
    std::cout << "The highest priority was not granted." << std::endl;
  }
  std::cout.flush();
}

struct Session {
  uint8_t output_recipe = 0;
  uint8_t input_recipe = 0;
};

Session handshake(Socket& socket, Reader& reader) {
  std::vector<uint8_t> payload;
  std::string error;

  std::vector<uint8_t> version;
  append_u16(version, 2);
  send_command(socket.fd(), kRequestProtocolVersion, version);
  if (!take_packet(reader, kRequestProtocolVersion, payload, error) || payload.size() != 1 || payload[0] != 1) {
    throw std::runtime_error(error.empty() ? "controller rejected protocol version 2" : error);
  }

  std::vector<uint8_t> outputs;
  append_f64(outputs, kFrequencyHz);
  outputs.insert(outputs.end(), kOutputNames, kOutputNames + sizeof(kOutputNames) - 1);
  send_command(socket.fd(), kSetupOutputs, outputs);
  if (!take_packet(reader, kSetupOutputs, payload, error)) {
    throw std::runtime_error(error);
  }
  uint8_t output_recipe = 0;
  const std::string output_types = recipe_types(payload, output_recipe);
  require_recipe("output", output_recipe, output_types, kOutputTypes);

  std::vector<uint8_t> inputs(kInputNames, kInputNames + sizeof(kInputNames) - 1);
  send_command(socket.fd(), kSetupInputs, inputs);
  if (!take_packet(reader, kSetupInputs, payload, error)) {
    throw std::runtime_error(error);
  }
  uint8_t input_recipe = 0;
  const std::string input_types = recipe_types(payload, input_recipe);
  require_recipe("input", input_recipe, input_types, kInputTypes);

  send_command(socket.fd(), kStart);
  if (!take_packet(reader, kStart, payload, error) || payload.size() != 1 || payload[0] != 1) {
    throw std::runtime_error(error.empty() ? "controller refused to start RTDE output" : error);
  }

  return Session{output_recipe, input_recipe};
}

bool read_output(const std::vector<uint8_t>& payload, uint8_t output_recipe, Row& row, std::string& error) {
  constexpr std::size_t kPayloadSize = 1 + 8 + 4 + 4 + 4;
  if (payload.size() != kPayloadSize) {
    error = "output package payload was " + std::to_string(payload.size()) + " bytes";
    return false;
  }
  if (payload[0] != output_recipe) {
    error = "output package recipe id was " + std::to_string(payload[0]);
    return false;
  }
  row.robot_timestamp = read_be<double>(payload.data() + 1);
  row.runtime_state = read_be<uint32_t>(payload.data() + 9);
  row.echo_counter = read_be<int32_t>(payload.data() + 13);
  row.script_counter = read_be<int32_t>(payload.data() + 17);
  return true;
}

void send_client_counter(int fd, uint8_t input_recipe, int32_t client_counter) {
  std::vector<uint8_t> payload;
  payload.push_back(input_recipe);
  append_i32(payload, client_counter);
  send_command(fd, kDataPackage, payload);
}

Run exchange(Socket& socket, Reader& reader, const Session& session, double duration_s) {
  Run run;
  run.socket_open = true;
  int32_t client_counter = 0;
  const auto end_at = std::chrono::steady_clock::now() + std::chrono::duration<double>(duration_s);

  if (duration_s > 0.0) {
    const auto capacity = static_cast<std::size_t>(duration_s * kFrequencyHz) + 64;
    run.rows.reserve(capacity);
  }

  while (std::chrono::steady_clock::now() < end_at) {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
    std::string error;
    if (!reader.next(type, payload, error)) {
      run.stop = Stop::Protocol;
      run.detail = error;
      return run;
    }
    if (type == kTextMessage) {
      run.stop = Stop::Protocol;
      run.detail = "controller text message: " + text_message(payload);
      return run;
    }
    if (type != kDataPackage) {
      run.stop = Stop::Protocol;
      run.detail = "expected a data package, received " + std::to_string(type);
      return run;
    }

    Row row;
    if (!read_output(payload, session.output_recipe, row, error)) {
      run.stop = Stop::Protocol;
      run.detail = error;
      return run;
    }
    if (row.runtime_state != kPlaying) {
      run.stop = Stop::RuntimeState;
      run.detail = "runtime_state is " + std::to_string(row.runtime_state);
      return run;
    }
    if (client_counter == INT32_MAX) {
      run.stop = Stop::Protocol;
      run.detail = "Client Counter reached the end of the int32 range";
      return run;
    }

    ++client_counter;
    try {
      send_client_counter(socket.fd(), session.input_recipe, client_counter);
    } catch (const std::exception& ex) {
      run.stop = Stop::Protocol;
      run.detail = ex.what();
      run.socket_open = false;
      socket.close();
      return run;
    }

    row.client_counter = client_counter;
    run.rows.push_back(row);
  }

  run.stop = Stop::Duration;
  run.detail = "duration elapsed";
  return run;
}

void pause_stream(Socket& socket, Reader& reader) {
  if (!socket.open()) {
    return;
  }
  try {
    send_command(socket.fd(), kPause);
    for (int ignored = 0; ignored < 1000; ++ignored) {
      uint8_t type = 0;
      std::vector<uint8_t> payload;
      std::string error;
      if (!reader.next(type, payload, error)) {
        std::cerr << "Pause did not complete: " << error << std::endl;
        return;
      }
      if (type == kPause) {
        if (payload.size() != 1 || payload[0] != 1) {
          std::cerr << "Controller refused the pause." << std::endl;
        }
        return;
      }
      if (type != kDataPackage) {
        std::cerr << "Pause stopped on package " << static_cast<int>(type) << "." << std::endl;
        return;
      }
    }
    std::cerr << "Pause did not complete: output packages kept arriving." << std::endl;
  } catch (const std::exception& ex) {
    std::cerr << "Pause did not complete: " << ex.what() << std::endl;
  }
}

void write_csv(const std::string& path, const std::vector<Row>& rows) {
  std::ofstream handle(path, std::ios::binary);
  if (!handle) {
    throw std::runtime_error("could not write " + path);
  }
  handle << "robot_timestamp,client_counter,echo_counter,script_counter,runtime_state\n";
  handle << std::fixed << std::setprecision(9);
  for (const Row& row : rows) {
    handle << row.robot_timestamp << ',' << row.client_counter << ',' << row.echo_counter << ','
           << row.script_counter << ',' << row.runtime_state << '\n';
  }
  if (!handle) {
    throw std::runtime_error("could not finish writing " + path);
  }
}

bool echo_belongs(const Row& row) {
  return row.echo_counter >= 1 && row.echo_counter <= row.client_counter;
}

void summarise(const Run& run) {
  const std::vector<Row>& rows = run.rows;
  std::cout << "samples                        " << rows.size() << std::endl;
  std::cout << "stop                           " << run.detail << std::endl;
  if (rows.size() < 2) {
    std::cout << "Too few samples to summarise the exchange." << std::endl;
    return;
  }

  const double span = rows.back().robot_timestamp - rows.front().robot_timestamp;
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "robot timestamp span           " << span << " s" << std::endl;
  if (span > 0.0) {
    const double rate = static_cast<double>(rows.size() - 1) / span;
    std::cout << "exchange rate                  " << std::setprecision(2) << rate << " Hz" << std::endl;
  }

  int repeats = 0;
  int jumps = 0;
  int skipped_steps = 0;
  int rewinds = 0;
  for (std::size_t i = 1; i < rows.size(); ++i) {
    const int64_t step =
        static_cast<int64_t>(rows[i].script_counter) - static_cast<int64_t>(rows[i - 1].script_counter);
    if (step == 0) {
      ++repeats;
    } else if (step > 1) {
      ++jumps;
      skipped_steps += static_cast<int>(step - 1);
    } else if (step < 0) {
      ++rewinds;
    }
  }
  std::cout << "script counter repeats         " << repeats << std::endl;
  std::cout << "output-side Missed Packages    " << jumps << std::endl;
  std::cout << "skipped Script Counter steps   " << skipped_steps << std::endl;
  if (rewinds != 0) {
    std::cout << "script counter rewinds         " << rewinds << std::endl;
  }

  int window = -1;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (echo_belongs(rows[i])) {
      window = static_cast<int>(i);
      break;
    }
  }

  int held = 0;
  int overtaken = 0;
  int comparable = 0;
  if (window >= 0) {
    for (std::size_t i = static_cast<std::size_t>(window) + 1; i < rows.size(); ++i) {
      const int64_t step =
          static_cast<int64_t>(rows[i].script_counter) - static_cast<int64_t>(rows[i - 1].script_counter);
      if (step != 1) {
        continue;
      }
      ++comparable;
      const int64_t advance =
          static_cast<int64_t>(rows[i].echo_counter) - static_cast<int64_t>(rows[i - 1].echo_counter);
      if (advance == 0) {
        ++held;
      } else if (advance > 1) {
        ++overtaken;
      }
    }
  }
  std::cout << "comparable script steps        " << comparable << std::endl;
  std::cout << "echo held                      " << held << std::endl;
  std::cout << "echo advanced by more than 1   " << overtaken << std::endl;

  const int32_t last_client = rows.back().client_counter;
  std::vector<char> seen(static_cast<std::size_t>(last_client) + 1, 0);
  int32_t max_echo = 0;
  for (const Row& row : rows) {
    if (!echo_belongs(row)) {
      continue;
    }
    seen[static_cast<std::size_t>(row.echo_counter)] = 1;
    if (row.echo_counter > max_echo) {
      max_echo = row.echo_counter;
    }
  }

  int missed = 0;
  if (max_echo == 0) {
    missed = last_client;
    std::cout << "echo never showed a Client Counter from this run" << std::endl;
  } else {
    for (int32_t value = 1; value <= max_echo; ++value) {
      if (seen[static_cast<std::size_t>(value)] == 0) {
        ++missed;
      }
    }
  }
  const int32_t not_yet = max_echo == 0 ? 0 : last_client - max_echo;
  std::cout << "inbound Missed Packages        " << missed << std::endl;
  std::cout << "client counters not yet echoed " << not_yet << std::endl;

  std::map<int64_t, int> skew_counts;
  int best = -1;
  int64_t resting = 0;
  for (const Row& row : rows) {
    const int64_t skew = static_cast<int64_t>(row.client_counter) - static_cast<int64_t>(row.script_counter);
    const int count = ++skew_counts[skew];
    if (count > best) {
      best = count;
      resting = skew;
    }
  }
  int64_t departure = 0;
  for (const auto& entry : skew_counts) {
    int64_t distance = entry.first - resting;
    if (distance < 0) {
      distance = -distance;
    }
    if (distance > departure) {
      departure = distance;
    }
  }
  std::cout << "resting Skew                  " << resting << std::endl;
  std::cout << "largest Skew departure        " << departure << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Args args = parse_args(argc, argv);
    std::cout << "Protocol Client at " << std::fixed << std::setprecision(0) << kFrequencyHz << " Hz, host "
              << args.host << ", duration " << std::setprecision(3) << args.duration_s << " s" << std::endl;
    Socket socket = connect_robot(args.host);
    raise_priority();
    Reader reader(socket.fd());
    const Session session = handshake(socket, reader);
    Run run = exchange(socket, reader, session, args.duration_s);
    if (run.socket_open) {
      pause_stream(socket, reader);
    }
    bool write_failed = false;
    try {
      write_csv(args.output, run.rows);
      std::cout << "wrote " << args.output << std::endl;
    } catch (const std::exception& ex) {
      write_failed = true;
      std::cerr << ex.what() << std::endl;
    }
    summarise(run);
    if (write_failed || run.stop != Stop::Duration) {
      return 1;
    }
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << ex.what() << std::endl;
    return 1;
  }
}
