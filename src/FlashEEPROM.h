#ifndef FLASHEEPROM_H
#define FLASHEEPROM_H

#include "basetypes.h"

//16 Mbit = 2 MByte
#define DEFAULT_EEPROM_SIZE (2 * 1024 * 1024)
#define DEFAULT_NUM_SECTORS (39)

///Device ID = AT49BV162A/163A
#define EEPROM_DEVICE_ID (0xC0)
///Manufacturer = Atmel
#define EEPROM_MANUFACTURER_CODE (0x1F)

#define EEPROM_STATE_READ (1)
#define EEPROM_STATE_BUS_CYCLE_2 (2)
#define EEPROM_STATE_BUS_CYCLE_3 (3)
#define EEPROM_STATE_BUS_CYCLE_4 (4)
#define EEPROM_STATE_BUS_CYCLE_5 (5)
#define EEPROM_STATE_BUS_CYCLE_6 (6)

enum class eFlashMode
{
  FlashMode_Read,
  FlashMode_CFI,
  FlashMode_SinglePulseProgram,
  FlashMode_Program,
  FlashMode_ProductID,
  FlashMode_Erase,
  FlashMode_Error
};

class FlashEEPROM final
{
public:
  FlashEEPROM()
  {
    eepromSize = DEFAULT_EEPROM_SIZE;
    init_nuon_mem(eeprom, DEFAULT_EEPROM_SIZE);
    //Device ID: AT49BV162A/163A
    eepromDeviceID = 0xC0;
    //Manufacturer Code : Atmel
    eepromManufacturerCode = 0x1F;
    Reset();
    LoadFromFile("FlashEEPROM.bin");
  }

  ~FlashEEPROM()
  {
    if(!saveFileName[0])
      SaveToFile("FlashEEPROM.bin");
    else if(bDirty)
      SaveToFile(saveFileName);
  }

  // For a frontend that says where saves go (the libretro core): from now on
  // the contents live in this file. It is read now if it exists, otherwise
  // legacyFileName is (a FlashEEPROM.bin from before saves were per game).
  // The constructor's own load is from the working directory of the moment,
  // which for a core loaded into a frontend is nowhere in particular.
  void UseSaveFile(const char * const fileName, const char * const legacyFileName);
  // Write the contents out if they have changed since they were last read or written.
  void SaveIfChanged();
  bool bDirty = false;

  uint8 *GetBasePointer()
  {
    return eeprom;
  }

  void Reset()
  {
    mode = eFlashMode::FlashMode_Read;
    errorMode = eFlashMode::FlashMode_Read;
    state = EEPROM_STATE_READ;
    errorState = EEPROM_STATE_READ;
    for(uint32 i = 0; i < DEFAULT_NUM_SECTORS; i++)
    {
      sectorLockStatus[i] = false;
    }
  }

  //Command functions
  void WriteData(uint32 address, uint32 data);
  void ReadData(uint32 address, uint32 *buffer);
  uint32 ReadStatus();

  //Sector functions
  bool IsSectorLocked(uint32 address);
  void EraseChip();
  void EraseSector(uint32 address);
  void LockSector(uint32 address);

  //File functions
  void LoadFromFile(const char * const fileName);
  void SaveToFile(const char * const fileName);

private:
  char saveFileName[1024] = {};
  uint32 eepromSize;
  uint8 eeprom[DEFAULT_EEPROM_SIZE];
  uint32 state;
  uint32 errorState;
  uint32 eepromManufacturerCode;
  uint32 eepromDeviceID;
  eFlashMode mode;
  eFlashMode errorMode;
  bool sectorLockStatus[DEFAULT_NUM_SECTORS];
};

#endif
