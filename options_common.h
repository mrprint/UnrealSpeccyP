/*
Portable ZX-Spectrum emulator.
Copyright (C) 2001-2010 SMT, Dexus, Alone Coder, deathsoft, djdron, scor

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __OPTIONS_COMMON_H__
#define __OPTIONS_COMMON_H__

#pragma once

namespace xPlatform
{

enum eJoystick { J_FIRST, J_CURSORENTER = J_FIRST, J_CURSOR, J_KEMPSTON, J_SINCLAIR2, J_QAOPM, J_QAOPSPACE, J_LAST };
enum eSound { S_FIRST, S_BEEPER = S_FIRST, S_AY, S_TAPE, S_LAST };
enum eSoundChipType { SC_FIRST, SC_AY = SC_FIRST, SC_YM, SC_LAST };
enum eMode { AS_FIRST, AS_ABC = AS_FIRST, AS_ACB, AS_BAC, AS_BCA, AS_CAB, AS_CBA, AS_MONO, AS_LAST };
enum eVolume { V_FIRST, V_MUTE = V_FIRST, V_10, V_20, V_30, V_40, V_50, V_60, V_70, V_80, V_90, V_100, V_LAST };
enum eDrive { D_FIRST, D_A = D_FIRST, D_B, D_C, D_D, D_LAST };

enum ePalEffects {
    PE_FIRST,
    PE_PAL_EFFECTS = PE_FIRST,
    PE_DOT_CRAWL,
    PE_PHASE_MODULATION,
    PE_LAST
};

const eJoystick DEFAULT_JOYSTICK = J_CURSORENTER;
const eSound DEFAULT_SOUND = S_AY;
const eSoundChipType DEFAULT_SOUND_CHIP = SC_AY;
const eMode DEFAULT_STEREO = AS_ABC;
const eVolume DEFAULT_VOLUME = V_50;
const eDrive DEFAULT_DRIVE = D_A;

const bool DEFAULT_AUTO_PLAY_IMAGE = true;
const bool DEFAULT_PAL_EFFECTS = true;
const bool DEFAULT_DOT_CRAWL = false;
const bool DEFAULT_PHASE_MODULATION = false;
const int DEFAULT_PAL_STRENGTH = 40; // 0-100 range
const int DEFAULT_BEAM_SPREAD = 15; // 0-200 range

const bool DEFAULT_FILTERING = true;
const bool DEFAULT_GIGASCREEN = false;
const bool DEFAULT_SCANLINES = false;
const float DEFAULT_ZOOM_VALUE = 1.0f;

const char* OpLastFolder();
const char* OpLastFile();
void OpLastFile(const char* path);

bool OpQuit();
void OpQuit(bool v);

eDrive OpDrive();
void OpDrive(eDrive d);

eJoystick OpJoystick();
void OpJoystick(eJoystick v);
dword OpJoyKeyFlags();

eVolume OpVolume();
void OpVolume(eVolume v);

eSound OpSound();
void OpSound(eSound s);

bool OpAutoPlayImage();
void OpAutoPlayImage(bool v);

bool OpPalEffects();
void OpPalEffects(bool v);
bool OpDotCrawl();
void OpDotCrawl(bool v);
bool OpPhaseMod();
void OpPhaseMod(bool v);
int OpPalStrength(); // 0-100 range
void OpPalStrength(int v);
int OpBeamSpread(); // 0-200 range  
void OpBeamSpread(int v);

}
//namespace xPlatform

#endif//__OPTIONS_COMMON_H__
