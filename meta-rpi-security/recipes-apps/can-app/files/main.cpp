#include <iostream>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <csignal>

#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <net/if.h>

#include <linux/can.h>
#include <linux/can/raw.h>

#include "vehicle-data.h"

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

// ============================================================
// Configuration
// ============================================================

// 1 = fake CAN data, 0 = real can0
#define TESTING_CAN 1

#define CAN_INTERFACE     "can0"
#define VEHICLE_SHM_NAME  "/vehicle_data"

#define CAN_ID_SPEED       0x100
#define CAN_ID_BATTERY     0x101
#define CAN_ID_TEMPERATURE 0x102
#define CAN_ID_RPM         0x103
#define CAN_ID_GEAR        0x104
#define CAN_ID_STATUS      0x105

static constexpr auto PUBLISH_PERIOD   = 100ms;  // shared memory update rate (10 Hz)
static constexpr auto TEST_PERIOD      = 100ms;  // fake data generation rate
static constexpr auto CAN_RETRY_PERIOD = 1000ms; // reconnect delay if can0 is down
static constexpr int  CAN_POLL_TIMEOUT = 100;    // ms, lets the thread notice shutdown

// ============================================================
// Global state shared between threads
// ============================================================

static std::atomic<bool> g_running{true};

// Latest vehicle state. Producer thread (CAN / test) writes,
// publisher thread reads. Always access under g_dataMutex.
static std::mutex  g_dataMutex;
static VehicleData g_vehicle{};

// Shared memory (only touched by the publisher thread after init)
static int          shm_fd = -1;
static VehicleData *sharedVehicleData = nullptr;

// ============================================================
// Signal handling (Ctrl+C / kill -> clean shutdown)
// ============================================================

static void onSignal(int)
{
    g_running = false;
}

// ============================================================
// Shared memory
// ============================================================

bool initSharedMemory()
{
    std::cout << "Creating shared memory: " << VEHICLE_SHM_NAME << "\n";

    shm_fd = shm_open(VEHICLE_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0)
    {
        perror("shm_open");
        return false;
    }

    if (ftruncate(shm_fd, sizeof(VehicleData)) < 0)
    {
        perror("ftruncate");
        close(shm_fd);
        shm_fd = -1;
        return false;
    }

    void *ptr = mmap(nullptr, sizeof(VehicleData),
                     PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (ptr == MAP_FAILED)
    {
        perror("mmap");
        close(shm_fd);
        shm_fd = -1;
        return false;
    }

    sharedVehicleData = static_cast<VehicleData *>(ptr);

    VehicleData initialData{};
    std::memcpy(sharedVehicleData, &initialData, sizeof(VehicleData));

    std::cout << "Shared memory created successfully\n"
              << "Name: " << VEHICLE_SHM_NAME << "\n"
              << "Size: " << sizeof(VehicleData) << " bytes\n";
    return true;
}

bool writeVehicleData(const VehicleData &d)
{
    if (sharedVehicleData == nullptr)
    {
        std::cerr << "ERROR: Shared memory is not initialized\n";
        return false;
    }
    std::memcpy(sharedVehicleData, &d, sizeof(VehicleData));
    return true;
}

void cleanupSharedMemory()
{
    if (sharedVehicleData != nullptr)
    {
        munmap(sharedVehicleData, sizeof(VehicleData));
        sharedVehicleData = nullptr;
    }

    if (shm_fd >= 0)
    {
        close(shm_fd);
        shm_fd = -1;
    }

    // Do NOT shm_unlink() here: the object stays available
    // so gpio-qt-demo can continue/reconnect.
}

// ============================================================
// Thread 1: Publisher  (loop: snapshot -> shared memory @ 10 Hz)
// ============================================================

void publisherThread()
{
    auto next = Clock::now();

    while (g_running)
    {
        VehicleData snapshot;
        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            snapshot = g_vehicle;
        }

        writeVehicleData(snapshot);

        next += PUBLISH_PERIOD;
        std::this_thread::sleep_until(next);
    }
}

// ============================================================
// CAN helpers
// ============================================================

void printCanFrame(const struct can_frame &frame)
{
    std::printf("CAN ID: 0x%03X DLC: %d DATA:",
                frame.can_id & CAN_SFF_MASK, frame.can_dlc);

    for (int i = 0; i < frame.can_dlc; ++i)
        std::printf(" %02X", frame.data[i]);

    std::printf("\n");
}

// Decode one frame into 'vehicle'. Caller must hold g_dataMutex.
// (Shared-memory writing is done by the publisher thread.)
void decodeCanFrame(const struct can_frame &frame, VehicleData &vehicle)
{
    uint32_t id = frame.can_id & CAN_SFF_MASK;

    // 0x100: bytes[0-1] = speed * 100
    if (id == CAN_ID_SPEED && frame.can_dlc >= 2)
    {
        uint16_t raw = static_cast<uint16_t>(frame.data[0] | (frame.data[1] << 8));
        vehicle.speed = raw / 100.0f;
    }
    // 0x101: byte0 = battery %, bytes1-2 = voltage*100, bytes3-4 = current*100
    else if (id == CAN_ID_BATTERY && frame.can_dlc >= 5)
    {
        vehicle.batteryPercent = frame.data[0];

        uint16_t rawV = static_cast<uint16_t>(frame.data[1] | (frame.data[2] << 8));
        uint16_t rawC = static_cast<uint16_t>(frame.data[3] | (frame.data[4] << 8));

        vehicle.batteryVoltage = rawV / 100.0f;
        vehicle.batteryCurrent = rawC / 100.0f;
    }
    // 0x102: bytes0-1 = temperature * 100 (signed)
    else if (id == CAN_ID_TEMPERATURE && frame.can_dlc >= 2)
    {
        int16_t raw = static_cast<int16_t>(frame.data[0] | (frame.data[1] << 8));
        vehicle.temperature = raw / 100.0f;
    }
    // 0x103: bytes0-1 = RPM
    else if (id == CAN_ID_RPM && frame.can_dlc >= 2)
    {
        uint16_t raw = static_cast<uint16_t>(frame.data[0] | (frame.data[1] << 8));
        vehicle.rpm = static_cast<float>(raw);
    }
    // 0x104: byte0 = gear
    else if (id == CAN_ID_GEAR && frame.can_dlc >= 1)
    {
        vehicle.gear = frame.data[0];
    }
    // 0x105: byte0 bits: 0=left, 1=right, 2=high beam, 3=ABS, 4=Bluetooth
    else if (id == CAN_ID_STATUS && frame.can_dlc >= 1)
    {
        uint8_t status = frame.data[0];

        vehicle.leftIndicator  = status & (1 << 0);
        vehicle.rightIndicator = status & (1 << 1);
        vehicle.highBeam       = status & (1 << 2);
        vehicle.abs            = status & (1 << 3);
        vehicle.bluetooth      = status & (1 << 4);
    }
}

#if TESTING_CAN

// ============================================================
// Thread 2: Fake CAN data generator (loop @ 10 Hz)
// ============================================================

void canThread()
{
    std::cout << "========================================\n"
              << " CAN APP\n"
              << " TEST MODE\n"
              << " TESTING_CAN = 1\n"
              << "========================================\n";

    float speed = 0.0f;
    auto next = Clock::now();

    while (g_running)
    {
        speed += 2.0f;
        if (speed > 160.0f)
            speed = 0.0f;

        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            VehicleData &vehicle = g_vehicle;

            vehicle.speed = speed;
            vehicle.rpm   = vehicle.speed * 65.0f;

            vehicle.batteryPercent = 78.0f - (vehicle.speed / 160.0f * 20.0f);
            vehicle.batteryVoltage = 12.0f + vehicle.batteryPercent * 0.008f;
            vehicle.batteryCurrent = 2.0f + vehicle.speed * 0.04f;

            vehicle.temperature = 55.0f + vehicle.speed * 0.18f;

            if      (vehicle.speed < 5)   vehicle.gear = 0;
            else if (vehicle.speed < 20)  vehicle.gear = 1;
            else if (vehicle.speed < 40)  vehicle.gear = 2;
            else if (vehicle.speed < 65)  vehicle.gear = 3;
            else if (vehicle.speed < 90)  vehicle.gear = 4;
            else if (vehicle.speed < 120) vehicle.gear = 5;
            else                          vehicle.gear = 6;

            vehicle.tripA   += vehicle.speed / 3600.0f * 0.1f;
            vehicle.tripB    = vehicle.tripA * 0.7f;
            vehicle.odometer = 1256.0f + vehicle.tripA;

            vehicle.averageSpeed = vehicle.speed * 0.7f;
            vehicle.maxSpeed     = std::max(vehicle.maxSpeed, vehicle.speed);
            vehicle.range        = vehicle.batteryPercent * 1.5f;

            vehicle.leftIndicator  = ((static_cast<int>(speed) / 2) % 10) == 0;
            vehicle.rightIndicator = false;
            vehicle.highBeam       = vehicle.speed > 110.0f;
            vehicle.abs            = true;
            vehicle.bluetooth      = true;

            if      (vehicle.speed < 60)  vehicle.driveMode = 0;
            else if (vehicle.speed < 110) vehicle.driveMode = 1;
            else                          vehicle.driveMode = 2;

            std::cout << "Speed=" << vehicle.speed
                      << " km/h | RPM=" << vehicle.rpm
                      << " | Gear=" << vehicle.gear
                      << " | Battery=" << vehicle.batteryPercent
                      << "% | Temp=" << vehicle.temperature
                      << " C\n";
        }

        next += TEST_PERIOD;
        std::this_thread::sleep_until(next);
    }
}

#else

// ============================================================
// Thread 2: Real CAN receiver (loop with auto-reconnect)
// ============================================================

// Returns a bound CAN socket, or -1 on failure.
static int openCanSocket()
{
    int sock = socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (sock < 0)
    {
        perror("socket");
        return -1;
    }

    struct ifreq ifr {};
    std::strncpy(ifr.ifr_name, CAN_INTERFACE, IFNAMSIZ - 1);

    if (ioctl(sock, SIOCGIFINDEX, &ifr) < 0)
    {
        perror("SIOCGIFINDEX");
        close(sock);
        return -1;
    }

    struct sockaddr_can addr {};
    addr.can_family  = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (bind(sock, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        perror("bind");
        close(sock);
        return -1;
    }

    return sock;
}

void canThread()
{
    std::cout << "========================================\n"
              << " CAN APP\n"
              << " REAL CAN MODE\n"
              << " Interface: " << CAN_INTERFACE << "\n"
              << "========================================\n";

    int sock = -1;

    while (g_running)
    {
        // ---- (Re)connect loop ----
        if (sock < 0)
        {
            sock = openCanSocket();

            if (sock < 0)
            {
                std::cerr << "CAN: " << CAN_INTERFACE << " not available, retrying...\n";
                std::this_thread::sleep_for(CAN_RETRY_PERIOD);
                continue;
            }

            std::cout << "Listening on " << CAN_INTERFACE << "...\n";
        }

        // ---- Wait for a frame (timeout so we can check g_running) ----
        struct pollfd pfd {};
        pfd.fd     = sock;
        pfd.events = POLLIN;

        int pr = poll(&pfd, 1, CAN_POLL_TIMEOUT);

        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            perror("poll");
            close(sock);
            sock = -1;
            continue;
        }

        if (pr == 0)
            continue; // timeout, loop again

        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        {
            std::cerr << "CAN socket error, reconnecting...\n";
            close(sock);
            sock = -1;
            continue;
        }

        // ---- Read + decode ----
        struct can_frame frame {};
        ssize_t nbytes = read(sock, &frame, sizeof(frame));

        if (nbytes < 0)
        {
            perror("CAN read");
            close(sock);
            sock = -1;
            continue;
        }

        if (nbytes != static_cast<ssize_t>(sizeof(frame)))
        {
            std::cerr << "Invalid CAN frame\n";
            continue;
        }

        printCanFrame(frame);

        {
            std::lock_guard<std::mutex> lock(g_dataMutex);
            decodeCanFrame(frame, g_vehicle);
        }
    }

    if (sock >= 0)
        close(sock);
}

#endif

// ============================================================
// MAIN
// ============================================================

int main()
{
    std::cout << "========================================\n"
              << " Vehicle CAN Application\n"
              << "========================================\n";

    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    // Shared memory must exist BEFORE the threads start
    if (!initSharedMemory())
    {
        std::cerr << "ERROR: Failed to initialize vehicle shared memory\n";
        return 1;
    }

    // Start worker threads
    std::thread can(canThread);
    std::thread publisher(publisherThread);

    std::cout << "Main application running... (Ctrl+C to stop)\n";

    // Main loop: keep the application alive, do housekeeping here
    while (g_running)
    {
        std::this_thread::sleep_for(200ms);
        // e.g. watchdog / health checks / other app logic
    }

    std::cout << "\nShutting down...\n";

    can.join();
    publisher.join();

    cleanupSharedMemory();

    return 0;
}