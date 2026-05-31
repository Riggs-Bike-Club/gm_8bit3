#define NO_MALLOC_OVERRIDE

#include <GarrysMod/Lua/Interface.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <scanning/symbolfinder.hpp>
#include <detouring/hook.hpp>
#include <iostream>
#include <iclient.h>
#include <unordered_map>
#include "ivoicecodec.h"
#include "audio_effects.h"
#include "net.h"
#include "thirdparty.h"
#include "steam_voice.h"
#include "eightbit_state.h"
#include <GarrysMod/Symbol.hpp>
#include <cstdint>
#include "opus_framedecoder.h"
#include <string>
#include <vector>
#include <memory>

#define STEAM_PCKT_SZ (sizeof(uint64_t) + sizeof(CRC32_t))

#ifdef SYSTEM_WINDOWS
#include <windows.h>

const std::vector<Symbol> BroadcastVoiceSyms = {
#if defined ARCHITECTURE_X86
		Symbol::FromSignature("\x55\x8B\xEC\xA1****\x83\xEC\x50\x83\x78\x30\x00\x0F\x84****\x53\x8D\x4D\xD8\xC6\x45\xB4\x01\xC7\x45*****"),
		Symbol::FromSignature("\x55\x8B\xEC\x8B\x0D****\x83\xEC\x58\x81\xF9****"),
#elif defined ARCHITECTURE_X86_64
		Symbol::FromSignature("\x48\x89\x5C\x24*\x56\x57\x41\x56\x48\x81\xEC****\x8B\xF2\x4C\x8B\xF1"),
#endif
};
#endif

#ifdef SYSTEM_LINUX
#include <dlfcn.h>
const std::vector<Symbol> BroadcastVoiceSyms = {
	Symbol::FromName("_Z21SV_BroadcastVoiceDataP7IClientiPcx"),
	Symbol::FromSignature("\x55\x48\x8D\x05****\x48\x89\xE5\x41\x57\x41\x56\x41\x89\xF6\x41\x55\x49\x89\xFD\x41\x54\x49\x89\xD4\x53\x48\x89\xCB\x48\x81\xEC****\x48\x8B\x3D****\x48\x39\xC7\x74\x25"),
};
#endif

static char decompressedBuffer[20 * 1024];
static char recompressBuffer[20 * 1024];

// Ãëîáàëüíûå / file-scope ïåðåìåííûå
Net* net_handl = nullptr;
EightbitState* g_eightbit = nullptr;

// Ôëàã ãîâîðèò, áûë ëè óñïåøíî ñîçäàí/âêëþ÷¸í detour íà SV_BroadcastVoice
static bool voiceHookAvailable = false;

#if defined(SYSTEM_WINDOWS) && defined(ARCHITECTURE_X86)
typedef void(__fastcall* SV_BroadcastVoiceData)(IClient* cl, int nBytes, char* data, int64 xuid);
#else
typedef void (*SV_BroadcastVoiceData)(IClient* cl, int nBytes, char* data, int64 xuid);
#endif

Detouring::Hook detour_BroadcastVoiceData;

#if defined(SYSTEM_WINDOWS) && defined(ARCHITECTURE_X86)
extern "C" void __fastcall hook_BroadcastVoiceData(IClient* cl, int nBytes, char* data, int64 xuid) {
#else
extern "C" void hook_BroadcastVoiceData(IClient * cl, int nBytes, char* data, int64 xuid) {
#endif
	// std::cerr << "[eightbit] hook_BroadcastVoiceData entered, nBytes=" << nBytes << "\n";
	// Áåçîïàñíîñòü: åñëè õóê ïî÷åìó-òî âûçâàí, íî ôëàã íå óñòàíîâëåí — âîçâðàùàåìñÿ â òðàíçèò.
	if (!voiceHookAvailable) {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}

	int uid = cl->GetUserID();

#ifdef THIRDPARTY_LINK
	if (checkIfMuted(cl->GetPlayerSlot() + 1)) {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
#endif

	auto& afflicted_players = g_eightbit->afflictedPlayers;
	if (g_eightbit->broadcastPackets && nBytes > sizeof(uint64_t)) {

#if defined ARCHITECTURE_X86
		uint64_t id64 = *(uint64_t*)((char*)cl + 181);
#else
		uint64_t id64 = *(uint64_t*)((char*)cl + 189);
#endif

		* (uint64_t*)decompressedBuffer = id64;
		size_t toCopy = nBytes - sizeof(uint64_t);
		std::memcpy(decompressedBuffer + sizeof(uint64_t), data + sizeof(uint64_t), toCopy);

		net_handl->SendPacket(g_eightbit->ip.c_str(), g_eightbit->port, decompressedBuffer, nBytes);
	}

	if (afflicted_players.find(uid) != afflicted_players.end()) {
		IVoiceCodec* codec = std::get<0>(afflicted_players.at(uid));

		if (nBytes < STEAM_PCKT_SZ) {
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		int bytesDecompressed = SteamVoice::DecompressIntoBuffer(codec, data, nBytes, decompressedBuffer, sizeof(decompressedBuffer));
		int samples = bytesDecompressed / 2;
		if (bytesDecompressed <= 0) {
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		int eff = std::get<1>(afflicted_players.at(uid));
		switch (eff) {
		case AudioEffects::EFF_BITCRUSH:
			AudioEffects::BitCrush((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_DESAMPLE:
			AudioEffects::Desample((uint16_t*)decompressedBuffer, samples);
			break;
			// Íîâûå ôèêñèðîâàííûå ïðåñåòû
		case AudioEffects::EFF_COMB:
			AudioEffects::ApplyComb((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_COMBO:
			AudioEffects::ApplyCombo((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_DARTHVADER:
			AudioEffects::ApplyDarthVader((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_RADIO:
			AudioEffects::ApplyRadio((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_ROBOT:
			AudioEffects::ApplyRobot((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_ALIEN:
			AudioEffects::ApplyAlien((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_OVERDRIVE:
			AudioEffects::ApplyOverdrive((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_DISTORTION:
			AudioEffects::ApplyDistortion((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_TELEPHONE:
			AudioEffects::ApplyTelephone((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_MEGAPHONE:
			AudioEffects::ApplyMegaphone((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_CHIPMUNK:
			AudioEffects::ApplyChipmunk((uint16_t*)decompressedBuffer, samples);
			break;
		case AudioEffects::EFF_SLOWMOTION:
			AudioEffects::ApplySlowMotion((uint16_t*)decompressedBuffer, samples);
			break;
		default:
			break;
		}

		uint64_t steamid = *(uint64_t*)data;
		int bytesWritten = SteamVoice::CompressIntoBuffer(steamid, codec, decompressedBuffer, samples * 2, recompressBuffer, sizeof(recompressBuffer), 24000);
		if (bytesWritten <= 0) {
			return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
		}

		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, bytesWritten, recompressBuffer, xuid);
	}
	else {
		return detour_BroadcastVoiceData.GetTrampoline<SV_BroadcastVoiceData>()(cl, nBytes, data, xuid);
	}
}

LUA_FUNCTION_STATIC(eightbit_crush) {
	g_eightbit->crushFactor = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_gain) {
	g_eightbit->gainFactor = (float)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_setbroadcastip) {
	g_eightbit->ip = std::string(LUA->GetString());
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_setbroadcastport) {
	g_eightbit->port = (uint16_t)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_broadcast) {
	g_eightbit->broadcastPackets = LUA->GetBool(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_getcrush) {
	LUA->PushNumber(g_eightbit->crushFactor);
	return 1;
}

LUA_FUNCTION_STATIC(eightbit_setdesamplerate) {
	g_eightbit->desampleRate = (int)LUA->GetNumber(1);
	return 0;
}

LUA_FUNCTION_STATIC(eightbit_enableEffect) {
	int id = LUA->GetNumber(1);
	int eff = LUA->GetNumber(2);

	auto& afflicted_players = g_eightbit->afflictedPlayers;
	if (afflicted_players.find(id) != afflicted_players.end()) {
		if (eff == AudioEffects::EFF_NONE) {
			IVoiceCodec* codec = std::get<0>(afflicted_players.at(id));
			delete codec;
			afflicted_players.erase(id);
		}
		else {
			std::get<1>(afflicted_players.at(id)) = eff;
		}
		return 0;
	}
	else if (eff != AudioEffects::EFF_NONE) {

		IVoiceCodec* codec = new SteamOpus::Opus_FrameDecoder();
		codec->Init(5, 24000);
		afflicted_players.insert(std::pair<int, std::tuple<IVoiceCodec*, int>>(id, std::tuple<IVoiceCodec*, int>(codec, eff)));
	}
	return 0;
}

GMOD_MODULE_OPEN()
{
	g_eightbit = new EightbitState();

	SourceSDK::ModuleLoader engine_loader("engine");
	SymbolFinder symfinder;

	void* sv_bcast = nullptr;

#ifdef SYSTEM_WINDOWS
	HMODULE hEngine = reinterpret_cast<HMODULE>(engine_loader.GetModule());
	if (hEngine == nullptr) {
		std::cerr << "[eightbit] engine module handle is null\n";
	}
	else {
#if defined(ARCHITECTURE_X86)
		const std::vector<Symbol> win32VoiceSyms = {
			Symbol::FromSignature("\x55\x8B\xEC\xA1****\x83\xEC\x50\x83\x78\x30\x00\x0F\x84****\x53\x8D\x4D\xD8\xC6\x45\xB4\x01\xC7\x45*****"),
			Symbol::FromSignature("\x55\x8B\xEC\x8B\x0D****\x83\xEC\x58\x81\xF9****"),
		};

		for (const auto& sym : win32VoiceSyms) {
			void* addr = symfinder.Resolve(hEngine, sym.name.c_str(), sym.length);
			if (addr != nullptr) {
				sv_bcast = addr;
				std::cerr << "[eightbit] Resolved SV_BroadcastVoiceData by Win32 signature at " << sv_bcast << "\n";
				break;
			}
			else {
				std::cerr << "[eightbit] Win32 signature attempt failed\n";
			}
		}
#elif defined(ARCHITECTURE_X86_64)
		const char* probeNames[] = {
			"SV_BroadcastVoice",
			"SV_BroadcastVoiceData",
			"SV_BroadcastVoicePacket",
			"SV_BroadcastVoiceToClients"
		};

		for (const char* pn : probeNames) {
			auto addr = reinterpret_cast<void*>(GetProcAddress(hEngine, pn));
			if (addr != nullptr) {
				sv_bcast = addr;
				std::cerr << "[eightbit] Found export via GetProcAddress: " << pn << " at " << sv_bcast << "\n";
				break;
			}
		}

		if (sv_bcast == nullptr) {
			std::vector<std::string> tryNames;
			tryNames.push_back("SV_BroadcastVoice");
			tryNames.push_back("SV_BroadcastVoiceData");
			tryNames.push_back("_Z21SV_BroadcastVoiceDataP7IClientiPcx");

			for (const auto& nm : tryNames) {
				Symbol nameSym = Symbol::FromName(nm.c_str());
				void* addr = symfinder.Resolve(engine_loader.GetModule(), nameSym.name.c_str(), nameSym.length);
				if (addr != nullptr) {
					sv_bcast = addr;
					std::cerr << "[eightbit] Resolved by name: " << nm << " at " << sv_bcast << "\n";
					break;
				}
				else {
					std::cerr << "[eightbit] Tried name: " << nm << " -> not found\n";
				}
			}
		}

		if (sv_bcast == nullptr) {
			for (const auto& sym : BroadcastVoiceSyms) {
				void* addr = symfinder.Resolve(engine_loader.GetModule(), sym.name.c_str(), sym.length);
				if (addr != nullptr) {
					sv_bcast = addr;
					std::cerr << "[eightbit] Resolved by signature at " << sv_bcast << "\n";
					break;
				}
				else {
					std::cerr << "[eightbit] Signature attempt failed (one of patterns)\n";
				}
			}
		}
#endif
	}
#endif

#ifdef SYSTEM_LINUX
	if (sv_bcast == nullptr) {
		for (const auto& sym : BroadcastVoiceSyms) {
			void* addr = symfinder.Resolve(engine_loader.GetModule(), sym.name.c_str(), sym.length);
			if (addr != nullptr) {
				sv_bcast = addr;
				std::cerr << "[eightbit] Resolved by Linux symbol/signature at " << sv_bcast << "\n";
				break;
			}
			else {
				std::cerr << "[eightbit] Linux symbol/signature attempt failed\n";
			}
		}
	}
#endif

	if (sv_bcast == nullptr) {
		std::cerr << "[eightbit] Could not locate SV_BroadcastVoice symbol — voice hook disabled. Module will run in degraded mode.\n";
		voiceHookAvailable = false;
	}
	else {
		try {
			detour_BroadcastVoiceData.Create(Detouring::Hook::Target(sv_bcast), reinterpret_cast<void*>(&hook_BroadcastVoiceData));

			if (!detour_BroadcastVoiceData.IsValid()) {
				std::cerr << "[eightbit] Invalid detour for SV_BroadcastVoiceData.\n";
				voiceHookAvailable = false;
			}
			else {
				detour_BroadcastVoiceData.Enable();
				voiceHookAvailable = true;
				std::cerr << "[eightbit] BroadcastVoice detour created and enabled.\n";
			}
		}
		catch (...) {
			std::cerr << "[eightbit] Exception while creating/enabling detour — voice hook disabled.\n";
			voiceHookAvailable = false;
		}
	}

	LUA->PushSpecial(GarrysMod::Lua::SPECIAL_GLOB);

	LUA->PushString("eightbit");
	LUA->CreateTable();

	LUA->PushString("SetCrushFactor");
	LUA->PushCFunction(eightbit_crush);
	LUA->SetTable(-3);

	LUA->PushString("GetCrushFactor");
	LUA->PushCFunction(eightbit_getcrush);
	LUA->SetTable(-3);

	LUA->PushString("EnableEffect");
	LUA->PushCFunction(eightbit_enableEffect);
	LUA->SetTable(-3);

	LUA->PushString("EnableBroadcast");
	LUA->PushCFunction(eightbit_broadcast);
	LUA->SetTable(-3);

	LUA->PushString("SetGainFactor");
	LUA->PushCFunction(eightbit_gain);
	LUA->SetTable(-3);

	LUA->PushString("SetDesampleRate");
	LUA->PushCFunction(eightbit_setdesamplerate);
	LUA->SetTable(-3);

	LUA->PushString("SetBroadcastIP");
	LUA->PushCFunction(eightbit_setbroadcastip);
	LUA->SetTable(-3);

	LUA->PushString("SetBroadcastPort");
	LUA->PushCFunction(eightbit_setbroadcastport);
	LUA->SetTable(-3);

	LUA->PushString("EFF_NONE");
	LUA->PushNumber(AudioEffects::EFF_NONE);
	LUA->SetTable(-3);

	LUA->PushString("EFF_DESAMPLE");
	LUA->PushNumber(AudioEffects::EFF_DESAMPLE);
	LUA->SetTable(-3);

	LUA->PushString("EFF_BITCRUSH");
	LUA->PushNumber(AudioEffects::EFF_BITCRUSH);
	LUA->SetTable(-3);

	LUA->PushString("EFF_COMB");
	LUA->PushNumber(AudioEffects::EFF_COMB);
	LUA->SetTable(-3);

	LUA->PushString("EFF_DARTHVADER");
	LUA->PushNumber(AudioEffects::EFF_DARTHVADER);
	LUA->SetTable(-3);

	LUA->PushString("EFF_RADIO");
	LUA->PushNumber(AudioEffects::EFF_RADIO);
	LUA->SetTable(-3);

	LUA->PushString("EFF_ROBOT");
	LUA->PushNumber(AudioEffects::EFF_ROBOT);
	LUA->SetTable(-3);

	LUA->PushString("EFF_ALIEN");
	LUA->PushNumber(AudioEffects::EFF_ALIEN);
	LUA->SetTable(-3);

	LUA->PushString("EFF_OVERDRIVE");
	LUA->PushNumber(AudioEffects::EFF_OVERDRIVE);
	LUA->SetTable(-3);

	LUA->PushString("EFF_DISTORTION");
	LUA->PushNumber(AudioEffects::EFF_DISTORTION);
	LUA->SetTable(-3);

	LUA->PushString("EFF_TELEPHONE");
	LUA->PushNumber(AudioEffects::EFF_TELEPHONE);
	LUA->SetTable(-3);

	LUA->PushString("EFF_MEGAPHONE");
	LUA->PushNumber(AudioEffects::EFF_MEGAPHONE);
	LUA->SetTable(-3);

	LUA->PushString("EFF_CHIPMUNK");
	LUA->PushNumber(AudioEffects::EFF_CHIPMUNK);
	LUA->SetTable(-3);

	LUA->PushString("EFF_SLOWMOTION");
	LUA->PushNumber(AudioEffects::EFF_SLOWMOTION);
	LUA->SetTable(-3);

	LUA->PushString("EFF_COMBO");
	LUA->PushNumber(AudioEffects::EFF_COMBO);
	LUA->SetTable(-3);

	LUA->SetTable(-3);
	LUA->Pop();

	net_handl = new Net();

#ifdef THIRDPARTY_LINK
	linkMutedFunc();
#endif

	return 0;
}

GMOD_MODULE_CLOSE()
{
	if (voiceHookAvailable) {
		detour_BroadcastVoiceData.Destroy();
	}

	for (auto& p : g_eightbit->afflictedPlayers) {
		IVoiceCodec* codec = std::get<0>(p.second);
		if (codec != nullptr) {
			delete codec;
		}
	}

	delete net_handl;
	delete g_eightbit;

	return 0;
}
