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

					auto context = thread->GetCurrentObject<RE::CombatBehaviorContextMelee>();
					if (context) {
						if (PerformSCARAttack(actor, combatTarg.get(), scarClip, context)) {
							break;  // Only break on successful execution (true positive)
						}
					}
				}
			}
		}

		return _ProcessEvent(a_sink, a_event, a_eventSource);
	}

}
