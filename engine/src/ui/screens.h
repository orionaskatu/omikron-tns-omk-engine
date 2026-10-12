// SPDX-License-Identifier: GPL-3.0-or-later
//
// ui/screens.h - the 37 interface SCREENS by name: the row index of the
// 92-byte screen table (`docs/UI.md` 3, `tables/ui.json`), which is what
// `ui.open` names and what every per-screen branch compares. The names are the
// table's own labels; a row the table marks `(ELIMINE)` is CUT - nothing opens
// it, and the constant exists so a number never stands alone.
#pragma once

namespace omk::screen {

inline constexpr int Videophone   = 0;   // VIDEOPHONE - the sneak's call
inline constexpr int Transcan     = 1;   // TRANSCAN (ELIMINE) - cut
inline constexpr int Multiplan    = 2;   // MULTIPLAN - the storage kiosk
inline constexpr int Appartement  = 3;   // APPARTEMENT (ELIMINE) - cut
inline constexpr int Lift         = 4;   // LIFT
inline constexpr int Terminal     = 5;   // TERMINAL - Kay'l's office terminal
inline constexpr int Journal      = 6;   // JOURNAL (ELIMINE) - cut
inline constexpr int Slider       = 7;   // SLIDER
inline constexpr int Puzzle       = 8;   // PUZZLE (ELIMINE) - cut
inline constexpr int Sneak        = 9;   // SNEAK - Kay'l's device
inline constexpr int FightNotSim  = 10;  // FIGHT_NOT_SIM (ELIMINE) - cut
inline constexpr int FightSim     = 11;  // FIGHT SIM - the training console
inline constexpr int GandharDoor  = 12;  // GANDHAR DOOR - the 6x6 combination
inline constexpr int Den          = 13;  // DEN - the locker's four wheels
inline constexpr int Xachen       = 14;  // XACHEN - Dakobah's cartridges
inline constexpr int SurvError    = 15;  // SURV ERROR
inline constexpr int SurvNoKit    = 16;  // SURV NO KIT
inline constexpr int SurvKit      = 17;  // SURV KIT
inline constexpr int Archives     = 18;  // ARCHIVES - the police files
inline constexpr int Morgue       = 19;  // MORGUE
inline constexpr int Bank         = 20;  // BANK - the first of the ten shops (20..28, 32)
inline constexpr int Pharmacie    = 21;
inline constexpr int Armurerie    = 22;
inline constexpr int Restaurant   = 23;
inline constexpr int Bar          = 24;
inline constexpr int Sorcellerie  = 25;
inline constexpr int Librairie    = 26;
inline constexpr int SexShop      = 27;
inline constexpr int Divers       = 28;
inline constexpr int StartMenu    = 29;  // OMK START MENU
inline constexpr int SaveGame     = 30;  // SAVE GAME - the save/load panel
inline constexpr int Pause        = 31;  // PAUSE GAME - the only screen that stops the world
inline constexpr int LibLahorey   = 32;  // LIB. LAHOREY - the tenth shop
inline constexpr int ShootMeca    = 33;  // SHOOT MECA
inline constexpr int ShootHuman   = 34;  // SHOOT HUMAN
inline constexpr int Options      = 35;  // OPTIONS
inline constexpr int HighScore    = 36;  // HIGH-SCORE - the shooting gallery's board (op 142)

}  // namespace omk::screen
