#ifndef VEHICLE_DATA_H
#define VEHICLE_DATA_H

#include <cstdint>

struct VehicleData
{
    float speed;
    float batteryPercent;
    float batteryVoltage;
    float batteryCurrent;

    float tripA;
    float tripB;
    float odometer;

    float averageSpeed;
    float maxSpeed;
    float range;

    float temperature;
    float rpm;

    int32_t gear;

    uint8_t leftIndicator;
    uint8_t rightIndicator;
    uint8_t highBeam;
    uint8_t abs;
    uint8_t bluetooth;

    char driveMode[16];

    uint32_t sequence;
};

#endif


