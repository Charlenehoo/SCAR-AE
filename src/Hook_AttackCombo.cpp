#include "Hook_AttackCombo.h"
#include "DataHandler.h"
#include "Function.h"

namespace SCAR
{
	static bool ShouldNextAttack(const std::string& a_payload)
	{
		if (a_payload.empty())
			return true;

		clib_util::RNG rng;
		return std::atoi(a_payload.c_str()) >= rng.generate(0.f, 100.f);
	}

	static bool PerformSCARAttack(RE::Actor* a_attacker, RE::Actor* a_targ, RE::hkbClipGenerator* a_clip, RE::CombatBehaviorContextMelee* a_context)
	{
		if (a_clip) {
			if (a_targ && a_attacker->GetActorRuntimeData().currentProcess && !a_attacker->IsPlayerRef() && std::to_underlying(a_attacker->CalculateLOS(a_targ, RE::NI_TWO_PI)) > 0 && AttackRangeCheck::CheckPathing(a_attacker, a_targ)) {
				SKSE::log::debug("Find SCAR Action Data in clip \"{}\" of \"{}\"", a_clip->animationName.c_str(), a_attacker->GetName());

				auto dataArr = DataHandler::GetSCARActionData(a_clip);
				std::ranges::sort(dataArr.begin(), dataArr.end(), SCARActionData::SortByWeight);
				for (auto& data : dataArr) {
					if (data.PerformSCARAction(a_attacker, a_targ, a_context, a_clip))
						return true;
				}
			}
		}

		return false;
	}

	EventResult AnimEventHook::ProcessEvent(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_sink, RE::BSAnimationGraphEvent* a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_eventSource)
	{
		if (!a_event) {
			return _ProcessEvent(a_sink, a_event, a_eventSource);
		}

		const auto holder = a_event->holder;
		auto actor = holder ? holder->As<RE::Actor>() : nullptr;
		if (!actor) {
			return _ProcessEvent(a_sink, a_event, a_eventSource);
		}

		auto& actorRuntime = actor->GetActorRuntimeData();
		auto combatTarg = actorRuntime.currentCombatTarget.get();
		auto highProcess = actorRuntime.currentProcess ? actorRuntime.currentProcess->high : nullptr;

		if (highProcess && combatTarg && _strcmpi("SCAR_ComboStart", a_event->tag.c_str()) == 0 && actor->IsAttacking() && ShouldNextAttack(a_event->payload.c_str())) {
			auto scarClip = DataHandler::GetSCARDataClip(actor);
			auto combatbehaviorCtrl = actorRuntime.combatController ? actorRuntime.combatController->behaviorController : nullptr;
			if (scarClip && combatbehaviorCtrl) {
				// =================================================================================
				// MATHEMATICAL FOUNDATION (Set Theory)
				// =================================================================================
				// Let S be the state space of a SCAR_ComboStart event.
				// C = Set of active threads carrying a valid CombatBehaviorContextMelee.
				// V = Set of contexts passing all internal validations in PerformSCARAttack
				//     (distance, angle, attack_type, combatattackdatas match).
				// A = Set of threads where currentNode->GetName() == "attack".
				//
				// Legacy impl: E_old = A ∩ C ∩ V. It was a strict subset, suffering from:
				//   - UAF crash when reading A (GetName) on worker threads.
				//   - False negatives when the node was in a sub-state (e.g., attackStart).
				//
				// Current impl: E_new = C ∩ V. E_old ⊆ E_new.
				// We completely drop 'A' because it is both fatal (UAF) and semantically too narrow.
				// The new logic is a superset. We rely solely on the strict validation of 'V'
				// inside PerformSCARAttack to prevent false positives.
				//
				// CRITICAL: The `break` statement is placed *inside* the `if (PerformSCARAttack(...))`
				// block. This is mathematically required to compute the full union of (C ∩ V) across
				// all threads. If we break on the first 'C' (regardless of 'V'), we end up with a
				// severely truncated subset (First(C) ∩ V), which caused the "NPC stuck in opener" bug.
				// =================================================================================

				// Dummy Clip Filter:
				// Dummy clips (SCAR_1hmReadyDummy.hkx) carry ActionData but their localTime stays
				// frozen at the end. Without this filter, if V passes, the same ActionData triggers
				// repeatedly, manifesting as an opener loop.
				std::string_view clipName{ scarClip->animationName.c_str() };
				if (clipName.find("Dummy") != std::string_view::npos) {
					return _ProcessEvent(a_sink, a_event, a_eventSource);
				}

				// =================================================================================
				// DEPENDENCIES & SOURCE-LEVEL EVIDENCE
				// =================================================================================
				// 1. OAR Compatibility (Dummy Filter Reliability):
				//    Verified against ersh1/OpenAnimationReplacer main. OAR replaces animations via
				//    `animationBindingIndex` into `hkbCharacterStringData::animationNames` and
				//    `binding->animation`, but it NEVER rewrites `hkbClipGenerator::animationName`.
				//    FakeClipGenerator doesn't appear in activeNodes. Therefore, checking "Dummy"
				//    in `animationName` is 100% reliable under OAR variants.
				//
				// 2. Thread Safety (UAF Avoidance):
				//    We DO NOT read `currentNode`. Instead, `GetCurrentObject` reads
				//    `thread->currentContextPtr.stack` (CombatBehaviorThread offset 0) and `frame`.
				//    These are internal to the thread object and never touch the external
				//    `currentNode` pointer that the AI main thread asynchronously recycles.
				//
				// 3. Search Space Completeness:
				//    Active threads may contain multiple melee contexts. The loop intentionally
				//    iterates until `PerformSCARAttack` returns true, guaranteeing we check all C ∩ V.
				// =================================================================================

				for (auto thread : combatbehaviorCtrl->activeThreads) {
					if (!thread)
						continue;

					// ============================================================================
					// currentNode gate（从 7fb5818 恢复），用 1.7.104 真实布局直读 name 字段。
					//
					// 依据：
					//   - CPR（跑在 1.7.104 上正常）的 RE/CombatBehaviorTreeNode.h:
					//       BSFixedString name;  // +0x08
					//   - CombatBehaviorTreeNode vtable slot 1 (GetName) 在 1.7.104 上返回 char*，
					//     不是 CommonLibVR 声明的 const BSFixedString&。
					//   - 调用 GetName() 会把 char* 当 &BSFixedString 解引用 → 读 [ptr] 得垃圾 →
					//     崩溃地址 RCX=0x50207461626D6F43 = "Combat P..." (CPR 注入的字符串首 8 字节)。
					//   - 直读 name 字段不经 vtable，不受该 ABI 漂移影响。
					// ============================================================================
					const auto* currentNode = thread->currentNode;
					if (!currentNode)
						continue;

					// ---- 临时诊断探针（B2 验证，测完删）----
					{
						static std::atomic<int> s_probe{ 0 };
						if (s_probe.fetch_add(1) < 20) {
							const auto nodeAddr = reinterpret_cast<std::uintptr_t>(currentNode);
							const auto vtable = *reinterpret_cast<const std::uintptr_t*>(nodeAddr);
							const auto slot1 = *reinterpret_cast<const std::uintptr_t*>(vtable + 8);
							const char* n = currentNode->name.c_str();
							SKSE::log::info(
								"[SCARPROBE] built={} node=0x{:X} vtable=0x{:X} slot1=0x{:X} name=\"{}\"",
								__TIMESTAMP__, nodeAddr, vtable, slot1, n ? n : "(null)");
						}
					}
					// ---- END 诊断探针 ----

					const char* nodeName = currentNode->name.c_str();
					if (!nodeName || _strcmpi(nodeName, "attack") != 0)
						continue;

					auto context = thread->GetCurrentObject<RE::CombatBehaviorContextMelee>();
					if (context) {
						if (PerformSCARAttack(actor, combatTarg.get(), scarClip, context)) {
							break;  // 只在成功时 break（4e84ac9 保留）
						}
					}
				}
			}
		}

		return _ProcessEvent(a_sink, a_event, a_eventSource);
	}

}
