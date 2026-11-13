#pragma once

namespace SCAR
{
	class AIAttackStartHook
	{
	public:
		static void InstallHook()
		{
			SKSE::AllocTrampoline(1 << 5);

			auto& trampoline = SKSE::GetTrampoline();

			auto base1 = RELOCATION_ID(48147, 49178).address();
			auto t1 = base1 + REL::Relocate(0x164, 0x148);
			auto base2 = RELOCATION_ID(48139, 49170).address();
			auto t2 = base2 + REL::Relocate(0x493, 0x3F1);
			auto t3 = base2 + REL::Relocate(0x4D7, 0x435);

			auto dump = [](const char* name, std::uintptr_t addr) {
				auto p = reinterpret_cast<std::uint8_t*>(addr);
				SKSE::log::info("{} @ {:#x}: {:02x} {:02x} {:02x} {:02x} {:02x}",
								name, addr, p[0], p[1], p[2], p[3], p[4]);
			};
			dump("CBAB+0x148", t1);
			dump("ADB+0x3F1 ", t2);
			dump("ADB+0x435 ", t3);

			_StartAttack = trampoline.write_call<5>(t1, StartAttack);
			SKSE::log::info("{} Done! trampoline addr = {:#x}", __FUNCTION__, _StartAttack.address());
		}

	private:
		static bool StartAttack(RE::CombatBehaviorContextMelee* a_context);

		static inline REL::Relocation<decltype(StartAttack)> _StartAttack;
	};
}
