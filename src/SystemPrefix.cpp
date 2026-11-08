// src/SystemPrefix.cpp
#include "SystemPrefix.h"

std::string get_fbneo_system_prefix(const std::string& game_system) {
    if (game_system == "NES") return "nes_";
    else if (game_system == "MSX 1") return "msx_";
    else if (game_system == "FDS" || game_system == "Nintendo FDS") return "fds_";
    else if (game_system == "Game Gear" || game_system == "Sega GameGear") return "gg_";
    else if (game_system == "Master System" || game_system == "Sega MasterSystem") return "sms_";
    else if (game_system == "Megadrive" || game_system == "Sega Megadrive Genesis") return "md_";
    else if (game_system == "Sega SG-1000" || game_system == "SG-1000") return "sg1k_";
    else if (game_system == "ColecoVision") return "cv_";
    else if (game_system == "ZX Spectrum" || game_system == "Sinclar Spectrum") return "spec_";
    else if (game_system == "NeoGeo Pocket" || game_system == "Neo Geo Pocket") return "ngp_";
    else if (game_system == "Fairchild Channel F") return "chf_";
    else if (game_system == "PC-Engine" || game_system == "NEC PC Engine") return "pce_";
    else if (game_system == "TurboGrafx 16" || game_system == "NEC TurboGraphX 16") return "tg_";
    else if (game_system == "SNES") return "snes_";
    else if (game_system == "SuprGrafx" || game_system == "NEC SGX") return "sgx_";
    else if (game_system == "GBA" || game_system == "Game Boy Advance") return "gba_";
    else if (game_system == "Astrocade Home Computer" || game_system == "Bally Astrocade") return "astro_";
    else if (game_system == "Atari 2600") return "a26_";
    return ""; // Arcade / Neo Geo have no prefix
}
