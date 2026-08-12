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

#include "platform/platform.h"
#include "platform/io.h"
#include "tools/options.h"
#include "ui/ui.h"
#include "options_common.h"
#include "file_type.h"

namespace xPlatform
{

static struct eOptionStoreSlot : public xOptions::eOptionEnum
{
	virtual const char* Name() const { return "save slot"; }
	virtual const char** Values() const
	{
		static const char* values[] = { "", "1", "2", "3", "4", "5", "6", "7", "8", "9", NULL };
		return values;
	}
	virtual void Change(bool next = true)
	{
		eOptionEnum::Change(0, 10, next);
	}
	virtual int Order() const { return 7; }
} op_save_slot;

struct eOptionSave : public xOptions::eOptionBool
{
	eOptionSave() { storeable = false; }
	virtual const char*	Value() const { return NULL; }
	bool PrepareName()
	{
		strcpy(name, OpLastFile());
		ext[0] = '\0';
		int l = strlen(name);
		if(!l || name[l - 1] == '/' || name[l - 1] == '\\')
			return false;
		for(const eFileType* t = eFileType::First(); t; t = t->Next())
		{
			char contain_path[xIo::MAX_PATH_LEN];
			char contain_name[xIo::MAX_PATH_LEN];
			if(t->Contain(name, contain_path, contain_name))
			{
				strcpy(name, contain_path);
				strcat(name, "#/");
				strcat(name, contain_name);
				l = strlen(name);
			}
		}
		char* e = name + l;
		while(e > name && *e != '.' && *e != '\\' && *e != '/')
			--e;
		if(*e != '.')
			return false;
		strcpy(ext, e);
		*e = '\0';
		while(e > name && *e != '@' && *e != '\\' && *e != '/')
			--e;
		if(*e == '@')
			*e = '\0';
		if(op_save_slot)
		{
			strcat(name, "@");
			strcat(name, op_save_slot.Value());
		}
		return true;
	}
	const char* FileName()
	{
		if(!PrepareName())
			return NULL;
		strcat(name, ext);
		return name;
	}
	const char* SnapshotName()
	{
		if(!PrepareName())
			return NULL;
		strcat(name, ".sna");
		return name;
	}
	char name[xIo::MAX_PATH_LEN];
	char ext[xIo::MAX_PATH_LEN];
};

static struct eOptionSaveFile : public eOptionSave
{
	virtual const char* Name() const { return "save file"; }
	virtual void Change(bool next = true)
	{
		const char* name = FileName();
		if(name)
			Set(Handler()->OnSaveFile(name));
		else
			Set(false);
	}
	virtual int Order() const { return 8; }
} op_save_file;

static struct eOptionSaveState : public eOptionSave
{
	virtual const char* Name() const { return "save state"; }
	virtual void Change(bool next = true)
	{
		const char* name = SnapshotName();
		if(name)
			Set(Handler()->OnSaveFile(name));
		else
			Set(false);
	}
	virtual int Order() const { return 5; }
} op_save_state;

static struct eOptionLoadState : public eOptionSave
{
	virtual const char* Name() const { return "load state"; }
	virtual void Change(bool next = true)
	{
		const char* name = SnapshotName();
		if(name)
			Set(Handler()->OnOpenFile(name));
		else
			Set(false);
	}
	virtual int Order() const { return 6; }
} op_load_state;

static struct eOptionTape : public xOptions::eOptionEnum
{
	eOptionTape() { storeable = false; }
	virtual const char* Name() const { return "tape"; }
	virtual const char** Values() const
	{
		static const char* values[] = { "n/a", "stop", "start", NULL };
		return values;
	}
	virtual void Change(bool next = true)
	{
		switch(Handler()->OnAction(A_TAPE_TOGGLE))
		{
		case AR_TAPE_NOT_INSERTED:	Set(0);	break;
		case AR_TAPE_STOPPED:		Set(1);	break;
		case AR_TAPE_STARTED:		Set(2);	break;
		default: break;
		}
	}
	virtual int Order() const { return 40; }
} op_tape;

static struct eOptionPause : public xOptions::eOptionBool
{
	eOptionPause() { storeable = false; }
	virtual const char* Name() const { return "pause"; }
	virtual void Change(bool next = true)
	{
		eOptionBool::Change();
		Handler()->VideoPaused(*this);
	}
	virtual int Order() const { return 70; }
} op_pause;

static struct eOptionSound : public xOptions::eOptionEnum
{
	eOptionSound() { Set(DEFAULT_SOUND); }
	virtual const char* Name() const { return "sound"; }
	virtual const char** Values() const
	{
		static const char* values[] = { "beeper", "ay", "tape", NULL };
		return values;
	}
	virtual void Change(bool next = true)
	{
		eOptionEnum::Change(S_FIRST, S_LAST, next);
	}
	virtual int Order() const { return 20; }
} op_sound;

static struct eOptionVolume : public xOptions::eOptionEnum
{
	eOptionVolume() { Set(DEFAULT_VOLUME); }
	virtual const char* Name() const { return "volume"; }
	virtual const char** Values() const
	{
		static const char* values[] = { "mute", "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%", NULL };
		return values;
	}
	virtual void Change(bool next = true)
	{
		eOptionEnum::Change(V_FIRST, V_LAST, next);
	}
	virtual int Order() const { return 30; }
} op_volume;

static struct eOptionAutoPlayImage : public xOptions::eOptionBool
{
	eOptionAutoPlayImage() { Set(DEFAULT_AUTO_PLAY_IMAGE); }
	virtual const char* Name() const { return "auto play image"; }
	virtual int Order() const { return 55; }
} op_auto_play_image;

eVolume	OpVolume() { return (eVolume)(int)op_volume; }
void OpVolume(eVolume v) { op_volume.Set(v); }

eSound	OpSound() { return (eSound)(int)op_sound; }
void OpSound(eSound s) { op_sound.Set(s); }

static struct eOptionJoy : public xOptions::eOptionEnum
{
	eOptionJoy() { Set(DEFAULT_JOYSTICK); storeable = true; }
	virtual const char* Name() const { return "joystick"; }
	virtual const char** Values() const
	{
		static const char* values[] = { "cursorenter", "cursor", "kempston", "sinclair2", "qaopm", "qaopspace", NULL };
		return values;
	}
	virtual void Change(bool next = true)
	{
		eOptionEnum::Change(J_FIRST, J_LAST, next);
	}
	virtual int Order() const { return 10; }
} op_joy;

static struct eOptionDrive : public xOptions::eOptionEnum
{
	eOptionDrive() { storeable = false; Set(DEFAULT_DRIVE); }
	virtual const char* Name() const { return "drive"; }
	virtual const char** Values() const
	{
		static const char* values[] = { "A", "B", "C", "D", NULL };
		return values;
	}
	virtual void Change(bool next = true)
	{
		eOptionEnum::Change(D_FIRST, D_LAST, next);
	}
	virtual int Order() const { return 60; }
} op_drive;

static struct eOptionReset : public xOptions::eOptionB
{
	eOptionReset() { storeable = false; }
	virtual const char* Name() const { return "reset"; }
	virtual void Change(bool next = true) { Handler()->OnAction(A_RESET); }
	virtual int Order() const { return 80; }
} op_reset;

static struct eOptionQuit : public xOptions::eOptionBool
{
	eOptionQuit() { storeable = false; }
	virtual const char* Name() const { return "quit"; }
	virtual int Order() const { return 100; }
	virtual const char** Values() const { return NULL; }
} op_quit;

static struct eOptionLastFile : public xOptions::eOptionString
{
	eOptionLastFile() { customizable = false; }
	virtual const char* Name() const { return "last file"; }
} op_last_file;

static struct eOptionMaskScale : public xOptions::eOptionInt {
    eOptionMaskScale() { Set(DEFAULT_MASK_SCALE); }
    virtual const char* Name() const { return "mask scale"; }
    virtual int Min() const { return 0; }   // 0 = disable mask
    virtual int Max() const { return 4; }   // Reasonable range (1-4 visible)
    virtual int Order() const { return 47; }
} op_mask_scale;

int OpMaskScale() { return static_cast<int>(op_mask_scale); }
void OpMaskScale(int v) { op_mask_scale.Set(v); }

const char* OpLastFile() { return op_last_file; }
const char* OpLastFolder()
{
	static char lf[xIo::MAX_PATH_LEN];
	strcpy(lf, OpLastFile());
	int l = strlen(lf);
	if(!l || lf[l - 1] == '\\' || lf[l - 1] == '/')
		return lf;
	char parent[xIo::MAX_PATH_LEN];
	xIo::GetPathParent(parent, lf);
	strcpy(lf, parent);
	strcat(lf, "/");
	return lf;
}
void OpLastFile(const char* name) { op_last_file.Set(name); }

bool OpQuit() { return op_quit; }
void OpQuit(bool v) { op_quit.Set(v); }

eDrive OpDrive() { return (eDrive)(int)op_drive; }
void OpDrive(eDrive d) { op_drive.Set(d); op_drive.Apply(); }

eJoystick OpJoystick() { return (eJoystick)(int)op_joy; }
void OpJoystick(eJoystick v) { op_joy.Set(v); op_joy.Apply(); }
dword OpJoyKeyFlags()
{
	switch(op_joy)
	{
	case J_CURSORENTER:	return KF_CURSORENTER;
	case J_CURSOR:		return KF_CURSOR;
	case J_KEMPSTON:	return KF_KEMPSTON;
	case J_SINCLAIR2:	return KF_SINCLAIR2;
	case J_QAOPM:		return KF_QAOPM;
	case J_QAOPSPACE:	return KF_QAOPSPACE;
	}
	return KF_QAOPM;
}

bool OpAutoPlayImage() { return op_auto_play_image; }
void OpAutoPlayImage(bool v) { op_auto_play_image.Set(v); }

static struct eOptionPalEffects : public xOptions::eOptionBool
{
	eOptionPalEffects() { Set(DEFAULT_PAL_EFFECTS); }
	virtual const char* Name() const { return "pal effects"; }
	virtual int Order() const { return 41; } // After scanlines
} op_pal_effects;

static struct eOptionPalStrength : public xOptions::eOptionInt
{
	eOptionPalStrength() { Set(DEFAULT_PAL_STRENGTH); } // Default: 50%
	virtual const char* Name() const { return "pal strength"; }
	virtual int Min() const { return 0; }
	virtual int Max() const { return 100; }
	virtual int Order() const { return 44; }
} op_pal_strength;

static struct eOptionBeamSpread : public xOptions::eOptionInt
{
	eOptionBeamSpread() { Set(DEFAULT_BEAM_SPREAD); } // Default: 30 (0-200 -> 0.0-2.0)
	virtual const char* Name() const { return "beam spread"; }
	virtual int Min() const { return 0; }
	virtual int Max() const { return 200; }
	virtual int Order() const { return 45; }
} op_beam_spread;

static struct eOptionNvidiaWarning : public xOptions::eOptionBool
{
    eOptionNvidiaWarning() { Set(DEFAULT_NVIDIA_WARNING); }
    virtual const char* Name() const { return "nvidia warning"; }
    virtual int Order() const { return 48; }
    virtual bool IsCustomizable() const { return true; }
} op_nvidia_warning;

bool OpPalEffects() { return op_pal_effects; }
void OpPalEffects(bool v) { op_pal_effects.Set(v); }

int OpPalStrength() { return (int)op_pal_strength; }
void OpPalStrength(int v) { op_pal_strength.Set(v); }

int OpBeamSpread() { return (int)op_beam_spread; }
void OpBeamSpread(int v) { op_beam_spread.Set(v); }

// --- SDL2 Gamepad options ---
static struct eOptionHostGamepad : public xOptions::eOptionInt {
    int player_index;
    // Name() must return a stable, per-instance string. A function-local
    // `static char buf[64]` here would be shared by BOTH player instances
    // (player 0 and player 1 call the very same method body), so calling
    // Name() on one player after having called it on the other silently
    // rewrites the first call's returned string out from under whoever's
    // still holding that pointer (e.g. option lookup/serialization code
    // that expects Name() to stay stable while it works) - producing
    // corrupted/mismatched option names. Store it as a real per-object
    // member instead.
    // Also: no parentheses - "(" and ")" are not valid characters in an
    // XML element name, and every other pre-existing option name in this
    // file uses only letters/digits/spaces (which the save/load layer
    // turns into underscores). Parens broke that round trip, so the
    // gamepad device index was written to disk correctly but silently
    // failed to be read back on the next launch.
    char name_buf[64];
    eOptionHostGamepad(int p) : player_index(p) {
        snprintf(name_buf, sizeof(name_buf), "host gamepad device %d", p);
        // eOptionInt's base constructor defaults to Set(0), which is a *valid*
        // SDL device index and would make JoystickProfile::IsEnabled() (host_device_index >= 0)
        // true out of the box, silently binding both players to physical device 0
        // before the user ever opens the config dialog. -1 means "no device assigned".
        Set(-1);
    }
    virtual const char* Name() const override { return name_buf; }
    virtual int Order() const override { return 15 + player_index; }
} op_host_gamepad_0(0), op_host_gamepad_1(1);

static struct eOptionJoystickMapping : public xOptions::eOptionString {
    int player_index;
    char name_buf[64];
    eOptionJoystickMapping(int p) : player_index(p) {
        snprintf(name_buf, sizeof(name_buf), "joystick mapping %d", p);
    }
    virtual const char* Name() const override { return name_buf; }
    virtual int Order() const override { return 16 + player_index; }
} op_joystick_mapping_0(0), op_joystick_mapping_1(1);

int OpHostGamepadDevice(int player) {
    if (player == 0) return static_cast<int>(op_host_gamepad_0);
    else return static_cast<int>(op_host_gamepad_1);
}

void OpHostGamepadDevice(int player, int device_index) {
    if (player == 0) op_host_gamepad_0.Set(device_index);
    else op_host_gamepad_1.Set(device_index);
}

std::string OpJoystickMappingData(int player) {
    const char* data = (player == 0) ? static_cast<const char*>(op_joystick_mapping_0)
                                     : static_cast<const char*>(op_joystick_mapping_1);
    return data ? std::string(data) : "";
}

void OpJoystickMappingData(int player, const std::string& data) {
    if (player == 0) op_joystick_mapping_0.Set(data.c_str());
    else op_joystick_mapping_1.Set(data.c_str());
}

}
//namespace xPlatform

