;	<<<< ----------------------------------------------------------------->>>>
;	<< [=============== Breed {tags} -> Interaction Entity ===============] >>
;	<<<<------------------------------------------------------------------>>>>

;	Requires GEM BreedTags (binaries\x64\dbghelp.dll). With it, a breed's {tags "..."} are real entity
;	tags from the moment the breed is applied, so {if tagged} sees them. Without the dll, breed tags are
;	only visible to Lua scene queries and nothing here fires (the inventory-item path still works).
;
;	This file turns those tags into the same skeleton / trait calls the inventory items used to make.
;	Tag names == item names, so breeds.inc / traits.inc stay the single list:
;
;		{tags "soldier" "sm_default_tx_00" "commissar_ID" "gasmask_ID"}
;
;	 - skeleton: any first column of breeds.inc  (gem_default_tx_00, sm_default_tx_00, tyr_warrior_tx_00 ...)
;	 - trait:    any entry of traits.inc          (commissar_ID, gasmask_ID, melee_ID, psyker_id ...)
;
;	Everything is idempotent (tag_skeleton_defined / bt_trait_* guards), so the spawn retries and the
;	Link_weapon call are harmless.

; ------------------------------------------------------------------------------------;
; // Selector back-ends (reached through "Skin_Tag_Selector" like Spawn / Inv_Item / Get_Props)

; // Breeds list:  %0 = breed tag (== item name), %1 = skeleton call name
(Define "Skin_Check_Event_Breed_Tag"
	else tagged "%0"
		{if tagged "tag_skeleton_defined"
		else
			{Call "skin_call_set_%1_Skin"}
		}
)

; // Traits list:  %0 = trait tag (== item name)
(Define "Skin_Check_Event_Breed_Tag_Trait"
	else tagged "%0"
		{if tagged "bt_trait_%0"
		else
			{tags add "bt_trait_%0"}
			{Call "skin_call_set_Trait_%0"}
		}
)

; ------------------------------------------------------------------------------------;

(define "Breed_Tags_Apply"
	{if not dead
		{if tagged "@@@@@@"	("Breed_Skin_Define_Get_Tags" args Breed_Tag)}
		{if tagged "@@@@@@"	("Trait_Skin_Define_Get_Tags" args Breed_Tag_Trait)}
	}
)

; // DEBUG - marks whether the breed tags were already on the entity at that moment ("soldier" is on every
; // breed). bt_probe_spawn_hit = dll working and tags present at spawn. Comment out once confirmed.
(define "Breed_Tags_Debug"
	{if tagged "soldier"
		{tags add "bt_probe_%w_hit"}
	else
		{tags add "bt_probe_%w_miss"}
	}
)

; // {on spawn}
(define "Breed_Tags_Define_Spawn_Event"
	("Breed_Tags_Debug" w(spawn))
	("Breed_Tags_Apply")
	{delay 0.1
		("Breed_Tags_Debug" w(spawn01))
		("Breed_Tags_Apply")
	}
	{delay 0.5	("Breed_Tags_Apply")}
)

; // {on "Link_weapon"} - runs BEFORE SC_H_Skin_Weapon_Define_Link_Weapon so a breed skeleton tag beats the
; // weapon-effector fallback (which would otherwise lock in GEM_Default / the weapon's skeleton first).
(define "Breed_Tags_Define_Link_Weapon_Event"
	("Breed_Tags_Debug" w(link))
	("Breed_Tags_Apply")
)
