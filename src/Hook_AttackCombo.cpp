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
				// Assumption 1: SCAR's own dummy clips (SCAR_1hmReadyDummy.hkx /
				//   SCAR_BlockIdleDummy.hkx) also carry SCAR_ActionData annotations, and their
				//   filenames contain "Dummy". The names are defined in the behavior's
				//   SCAR_1HM_Ready_StateMachine / SCAR_BlockIdleStateMachine, and listed in
				//   defaultfemale/#0029.txt and defaultmale/#0029.txt (animationNames).
				// Assumption 1b: A full-repo grep for "Dummy" finds exactly two SCAR clip files
				//   (SCAR_1hmReadyDummy.hkx, SCAR_BlockIdleDummy.hkx). SCAR_UpdateDummy is an
				//   event name, and SCAR_1HM_Ready_Dummy0/1 / SCAR_BlockIdleDummy0/1 are
				//   behavior node names; neither is returned by GetSCARDataClip as a clip.
				//   If a future SCAR version adds a dummy clip without "Dummy" in its
				//   filename, this filter misses it.
				// Assumption 2: DataHandler::GetSCARDataClip selects a clip solely by the
				//   presence of SCAR_ActionData annotations. It does not distinguish clip
				//   type, nor check whether the actor is currently in an attack state.
				// Assumption 3: Dummy clips are MODE_SINGLE_PLAY; their localTime stays at the
				//   end of the clip and does not advance. If an ActionData's
				//   triggerStartTime/triggerEndTime covers that end value, the same ActionData
				//   passes the time check on every SCAR_ComboStart, which manifests as an NPC
				//   stuck in its opener loop, repeating the same move.
				// Contract 1: Filtering by name here is a stopgap. If upstream renames the dummy
				//   clips, changes GetSCARDataClip's selection logic, or stops letting
				//   ActionData's triggerEndTime cover the dummy's end time, this filter must be
				//   updated or removed accordingly.
				// Contract 1b (verified): OAR does not rewrite hkbClipGenerator::animationName.
				//   It replaces animations via animationBindingIndex pointing into
				//   hkbCharacterStringData::animationNames (ReplacerMods.cpp:1505-1520) and
				//   binding->animation, but leaves animationName at the behavior-authored path.
				//   Verified against ersh1/OpenAnimationReplacer main:
				//   - ModAPI.cpp GetCurrentReplacementAnimationInfo returns
				//     ReplacementAnimation::GetAnimPath() / Variant::GetFilename(), not animationName.
				//   - ActiveClip.cpp ReplaceActiveAnimation only touches animationBindingIndex and
				//     binding->animation.
				//   - FakeClipGenerator ctor does not copy animationName, and FakeClipGenerator is
				//     not present in activeNodes, so GetSCARDataClip never sees it.
				//   Therefore "Dummy" in animationName remains reliable under OAR variant replacement.
				std::string_view clipName{ scarClip->animationName.c_str() };
				if (clipName.find("Dummy") != std::string_view::npos) {
					return _ProcessEvent(a_sink, a_event, a_eventSource);
				}

				// Contract 2: Do NOT read currentNode->GetName() here. This function runs on
				//   the Havok Behavior worker thread (BSJobs). The CombatBehaviorTreeNode
				//   pointed to by currentNode is asynchronously recycled by the AI main thread;
				//   calling GetName() triggers a use-after-free (confirmed by crash addresses
				//   such as 0x50207461626D6F33, which decodes to a "Combat P..." string
				//   allocated into the recycled ScrapHeap block).
				// Contract 3: GetCurrentObject<CombatBehaviorContextMelee>() reads
				//   thread->currentContextPtr.stack (CombatBehaviorThread offset 0) and frame,
				//   both of which are thread-internal and never touch currentNode. As long as
				//   the thread pointer comes from activeThreads and is non-null, this read is
				//   safe.
				// Contract 4: We deliberately drop the "thread is on the attack node"
				//   narrowing. Outside an attack, frame does not point to a melee context and
				//   GetCurrentObject returns null. Under concurrency, the first thread with a
				//   melee context is not necessarily the thread owning the current animation;
				//   attack_type and combatattackdatas are further validated inside
				//   PerformSCARAttack. If false positives ever appear, restore thread-level
				//   narrowing using value-type fields (thread->threadID / thread->state), NOT
				//   by going back to currentNode.
				for (auto thread : combatbehaviorCtrl->activeThreads) {
					if (!thread)
						continue;

					auto context = thread->GetCurrentObject<RE::CombatBehaviorContextMelee>();
					if (context) {
						PerformSCARAttack(actor, combatTarg.get(), scarClip, context);
						break;
					}
				}
			}
		}

		return _ProcessEvent(a_sink, a_event, a_eventSource);
	}

}
