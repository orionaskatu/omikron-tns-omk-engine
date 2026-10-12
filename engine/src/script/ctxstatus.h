// SPDX-License-Identifier: GPL-3.0-or-later
//
// script/ctxstatus.h - a script context's STATUS WORD by name.
//
// The word at context `+22`. `Script_Execute` (0x00406460) runs `while
// (status == 1)`, so any other value PARKS the context with its pc and stack
// intact until something writes 1 back - and nothing polls: every resume is an
// event or a pump step. `docs/SCRIPT_VM.md` "The context status word" has the
// table this transcribes, with each value's writers and what resumes it.
#pragma once

namespace omk::ctx_status {

inline constexpr int Idle            = 0;   // not running; `end` writes it, the action queue may arm
inline constexpr int Running         = 1;   // `Script_ProcessActions` and every resume
inline constexpr int InFight         = 3;   // op 62 `fight.begin`; event 2 resumes the FIRST at 3
inline constexpr int WaitObject      = 4;   // a scene object or a player move (ops 46/58/60/89); event 3
inline constexpr int Superseded      = 5;   // a transition caller a second transition replaced; nothing resumes it
inline constexpr int WaitScreen      = 6;   // op 70 `ui.open`; event 5, the screen's answer or close
inline constexpr int WaitCamera      = 7;   // a camera move with travel (ops 96/126); event 4
inline constexpr int WaitPreload     = 8;   // op 45 `area.preload`; `Script_Pump`'s tail once the load reports
inline constexpr int RetryTransition = 9;   // a refused `area.goto`; `Script_ProcessActions`' head writes 1
inline constexpr int InTransition    = 10;  // `Area_Transition`; the pump hands it back to mode 3
inline constexpr int TransitionDone  = 11;  // the 60-second watchdog's last step; event 3 links the decor

}  // namespace omk::ctx_status
