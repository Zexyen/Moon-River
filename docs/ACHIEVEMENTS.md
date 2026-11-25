# Achievements (Recomp Achievements)

Moon River integrates with [Recomp Achievements](https://github.com/elripalda/RecompAchievements) (`runtime/achievements.c`, `runtime/achievements.h`, `games/bayonetta2/runtime/game_shim_eh.c`). Started without `RL_SESSION`, every entry point is a complete no-op.

- **Total**: **50** achievements (**1,710** points):
  - **17 Story Chapter Clears** (IDs `1..17`, 255 pts — 15 pts each, any difficulty).
  - **3 Game Completion / Difficulty Achievements** (IDs `18..20`, 225 pts — higher difficulties retroactively unlock lower ones).
  - **2 All-Chapter Mastery Achievements** (IDs `21`, `50`, 250 pts — all Platinum on any difficulty; all Pure Platinum on ∞ Climax).
  - **17 ∞ Climax Pure Platinum Chapter Achievements** (IDs `22..38`, 425 pts — 25 pts each, Pure Platinum on ∞ Climax only).
  - **2 Secret Boss & Boss Mastery Achievements** (IDs `39..40`, 150 pts — Rodin & Balder No-Damage).
  - **4 Arsenal, Techniques & Exploration Achievements** (IDs `41..44`, 200 pts — all 13 Bayonetta 2 weapons, all Umbran Techniques, all 22 Muspelheim challenges, and all Umbra Witches' final resting places).
  - **5 Witch Trial Achievements** (IDs `45..49`, 205 pts — Witch Trials I–V).
- **Unlocks & saves**: Live unlocks fire through post-lift hooks in `f_7100d943b4` (chapter clear / Play History commit), `f_7100d9045c` (story Bewitchments), and `f_7100d8fa50` / `f_7100d96c68` (weapon unlocks). Pre-existing progress loaded from `SAVE:/save*.sav` (excluding `savesystem.sav`) opens a 180-frame grace window via `achievements_save_loaded()` and is reconciled from `save_obj` (`*(uint64_t *)0x71018668d0`) chapter slots (`save_obj + 500 + diff * 0x804 + ch * 0x4c`, best medal at `+16`, clear state at `+68`), Bewitchment bits (`save_obj + 432`), and weapon bits (`save_obj + 0xa46c`), reported with `earlier: true`.
- **Rich Presence**: Updated every 64 frames via `rl_ach_presence()` from `game_presence[4]` (current chapter from `f_7100b22a60`, difficulty from `save_obj + 492` or each Witch Trial's fixed difficulty).

---

## Game Completion & Mastery (5)

| ID | Title | Pts | Condition |
|---:|---|---:|---|
| 18 | Sovereign Power | 50 | Complete all 17 story chapters on any difficulty (1st Climax, 2nd Climax, 3rd Climax, or ∞ Climax). |
| 19 | 3rd Climax Witch | 75 | Complete all 17 story chapters on **3rd Climax** or higher (also unlocks `#18`). |
| 20 | Infinite Climax | 100 | Complete all 17 story chapters on **∞ Climax** (highest difficulty; retroactively unlocks `#19` and `#18`). |
| 21 | Platinum Witch | 100 | Earn a **Platinum** or **Pure Platinum** award on all 17 story chapters (any difficulty). |
| 50 | Pure Platinum Witch | 150 | Earn a **Pure Platinum** award on all 17 story chapters on **∞ Climax**. |

## Secret Boss & Boss Mastery (2)

| ID | Source | Title | Pts | Objective |
|---:|---|---|---:|---|
| 39 | Bewitchment `#29` | Devil's Duel | 100 | Defeat Rodin in the Platinum Ticket battle. |
| 40 | Bewitchment `#25` | Darkness Envelops Light | 50 | Defeat Balder in Chapter XII without taking damage. |

## Arsenal, Techniques & Exploration (4)

| ID | Source | Title | Pts | Objective |
|---:|---|---|---:|---|
| 41 | `save_obj + 0xa46c` (`0xff8f0000`) | Arsenal of Darkness | 50 | Unlock all 13 Bayonetta 2 weapons (Love Is Blue, Rakshasa, Takemikazuchi, Alruna, Kafka, Chernobog, Chain Chomp, Undine, Salamandra, Handguns, Scarborough Fair, Shuraba, and Rodin; excludes the temporary Star Fox costume weapon Arwing). |
| 42 | Bewitchment `#18` | Umbran Successor | 50 | Acquire all Umbran Techniques. |
| 43 | Bewitchment `#23` | End Ruler | 50 | Complete all 22 Muspelheim challenges. |
| 44 | Bewitchment `#16` | Liberator of Time | 50 | Open all of the Umbra Witches' final resting places. |

## Witch Trials (5)

| ID | Chapter | Title | Pts | Objective |
|---:|---|---|---:|---|
| 45 | `22` | Witch Trial I Clear | 20 | Complete Witch Trial I. |
| 46 | `23` | Witch Trial II Clear | 25 | Complete Witch Trial II. |
| 47 | `24` | Witch Trial III Clear | 35 | Complete Witch Trial III. |
| 48 | `25` | Witch Trial IV Clear | 50 | Complete Witch Trial IV. |
| 49 | `26` | Witch Trial V Clear | 75 | Complete Witch Trial V. |

## Story Chapter Clears (17)

Unlocked on any difficulty (1st Climax, 2nd Climax, 3rd Climax, or ∞ Climax):

| ID | Chapter | Title | Pts | Objective |
|---:|---|---|---:|---|
| 1 | `1` | Prologue Clear | 15 | Complete Prologue: World of Chaos on any difficulty. |
| 2 | `2` | Chapter I Clear | 15 | Complete Chapter I: Noatun, The City of Genesis on any difficulty. |
| 3 | `3` | Chapter II Clear | 15 | Complete Chapter II: A Remembrance of Time on any difficulty. |
| 4 | `4` | Chapter III Clear | 15 | Complete Chapter III: Paradiso - The Gates of Paradise on any difficulty. |
| 5 | `5` | Chapter IV Clear | 15 | Complete Chapter IV: The Two Meet on any difficulty. |
| 6 | `6` | Chapter V Clear | 15 | Complete Chapter V: The Cathedral of Cascades on any difficulty. |
| 7 | `7` | Chapter VI Clear | 15 | Complete Chapter VI: The Bridge to the Heavens on any difficulty. |
| 8 | `8` | Chapter VII Clear | 15 | Complete Chapter VII: The Ark on any difficulty. |
| 9 | `9` | Chapter VIII Clear | 15 | Complete Chapter VIII: An Ancient Civilization on any difficulty. |
| 10 | `10` | Chapter IX Clear | 15 | Complete Chapter IX: The Gates of Hell on any difficulty. |
| 11 | `11` | Chapter X Clear | 15 | Complete Chapter X: The Depths on any difficulty. |
| 12 | `12` | Chapter XI Clear | 15 | Complete Chapter XI: Inferno and Its Ruler on any difficulty. |
| 13 | `13` | Chapter XII Clear | 15 | Complete Chapter XII: The Lumen Sage on any difficulty. |
| 14 | `14` | Chapter XIII Clear | 15 | Complete Chapter XIII: Vigrid, City of Déjà Vu on any difficulty. |
| 15 | `15` | Chapter XIV Clear | 15 | Complete Chapter XIV: The Witch Hunts on any difficulty. |
| 16 | `16` | Chapter XV Clear | 15 | Complete Chapter XV: Truth on any difficulty. |
| 17 | `17` | Chapter XVI Clear | 15 | Complete Chapter XVI: Sovereign Power on any difficulty. |

## ∞ Climax Pure Platinum Chapter Mastery (17)

Unlocked **only on ∞ Climax** with a **Pure Platinum** award:

| ID | Chapter | Title | Pts | Objective |
|---:|---|---|---:|---|
| 22 | `1` | Prologue: Pure Climax | 25 | Earn a Pure Platinum award in Prologue: World of Chaos on ∞ Climax. |
| 23 | `2` | Chapter I: Pure Climax | 25 | Earn a Pure Platinum award in Chapter I: Noatun, The City of Genesis on ∞ Climax. |
| 24 | `3` | Chapter II: Pure Climax | 25 | Earn a Pure Platinum award in Chapter II: A Remembrance of Time on ∞ Climax. |
| 25 | `4` | Chapter III: Pure Climax | 25 | Earn a Pure Platinum award in Chapter III: Paradiso - The Gates of Paradise on ∞ Climax. |
| 26 | `5` | Chapter IV: Pure Climax | 25 | Earn a Pure Platinum award in Chapter IV: The Two Meet on ∞ Climax. |
| 27 | `6` | Chapter V: Pure Climax | 25 | Earn a Pure Platinum award in Chapter V: The Cathedral of Cascades on ∞ Climax. |
| 28 | `7` | Chapter VI: Pure Climax | 25 | Earn a Pure Platinum award in Chapter VI: The Bridge to the Heavens on ∞ Climax. |
| 29 | `8` | Chapter VII: Pure Climax | 25 | Earn a Pure Platinum award in Chapter VII: The Ark on ∞ Climax. |
| 30 | `9` | Chapter VIII: Pure Climax | 25 | Earn a Pure Platinum award in Chapter VIII: An Ancient Civilization on ∞ Climax. |
| 31 | `10` | Chapter IX: Pure Climax | 25 | Earn a Pure Platinum award in Chapter IX: The Gates of Hell on ∞ Climax. |
| 32 | `11` | Chapter X: Pure Climax | 25 | Earn a Pure Platinum award in Chapter X: The Depths on ∞ Climax. |
| 33 | `12` | Chapter XI: Pure Climax | 25 | Earn a Pure Platinum award in Chapter XI: Inferno and Its Ruler on ∞ Climax. |
| 34 | `13` | Chapter XII: Pure Climax | 25 | Earn a Pure Platinum award in Chapter XII: The Lumen Sage on ∞ Climax. |
| 35 | `14` | Chapter XIII: Pure Climax | 25 | Earn a Pure Platinum award in Chapter XIII: Vigrid, City of Déjà Vu on ∞ Climax. |
| 36 | `15` | Chapter XIV: Pure Climax | 25 | Earn a Pure Platinum award in Chapter XIV: The Witch Hunts on ∞ Climax. |
| 37 | `16` | Chapter XV: Pure Climax | 25 | Earn a Pure Platinum award in Chapter XV: Truth on ∞ Climax. |
| 38 | `17` | Chapter XVI: Pure Climax | 25 | Earn a Pure Platinum award in Chapter XVI: Sovereign Power on ∞ Climax. |
