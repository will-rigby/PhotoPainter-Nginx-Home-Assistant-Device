// SPDX-License-Identifier: 0BSD

// --------------------------------------------------
// SD Card (SPI mode on HSPI bus)
// --------------------------------------------------

SPIClass sdSpi(HSPI);

bool sdInit() {
  Serial.println("Initializing SD card...");

  sdSpi.begin(SD_PIN_CLK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);

  if (!SD.begin(SD_PIN_CS, sdSpi, 4000000)) {
    Serial.println("SD card init failed!");
    return false;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card detected!");
    return false;
  }

  Serial.print("SD card type: ");
  switch (cardType) {
    case CARD_MMC:  Serial.println("MMC");  break;
    case CARD_SD:   Serial.println("SD");   break;
    case CARD_SDHC: Serial.println("SDHC"); break;
    default:        Serial.println("Unknown"); break;
  }

  Serial.print("SD card size: ");
  Serial.print(SD.cardSize() / (1024 * 1024));
  Serial.println(" MB");

  return true;
}
