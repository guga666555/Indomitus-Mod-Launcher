
;	<<<< ---------------------------------------------------------------------->>>>			 
;	<< [=============== Interaction Entity: Human ===============] >>
;	<<<<----------------------------------------------------------------------->>>>			 

{"prop_human"
	(Include "../40k_animations/SC_h_animations.inc")
	
	(Include "skin_weapon.inc")
	(Include "skin.inc")
	(Include "skin/breeds_traits/breed_tags.inc")	; // breed {tags} -> skeleton / trait calls (needs GEM BreedTags dll)
	
	;//(Include "crew.inc")
	;//(Include "crew_restrictions.inc")
	;//(Include "crew_bail.inc")
	
	{on spawn 
		{Delay 0.01 {volumes enable contact}}
		("Define_Skin_Visibility_Guard")		; // hidden skin bone = not skinned, whatever the tags say
		("Define_Spawn_Event")
		("Breed_Tags_Define_Spawn_Event")		; // breed {tags} path (see breed_tags.inc)
		
		{tags remove "spawned"} ; ///////////////////// ###########################
		("Define_Spawn_Event_Reload_Skins")
		(mod "gameclass:campaign_capture_the_flag" ("Define_Spawn_Stale_Skin_Check"))	; // conquest only: previewed veterans load with a stale "tag_skeleton_defined"
		
	}
	{on "Link_weapon" sync
		("Breed_Tags_Define_Link_Weapon_Event")	; // before the weapon-effector fallback so a breed skeleton tag wins
		("SC_H_Skin_Weapon_Define_Link_Weapon")		
		("SC_H_Request_Define_Link_Weapon")

		; // BUZZ: Patch up ork skins fingers
		{ani_play "orkboy_fidget_left_hand_choppa"}
		{ani_play "orkboy_fidget_right_hand_shoota"}
		{ani_play "orknob_fidget_left_hand_choppa"}
		{ani_play "orknob_fidget_right_hand_shoota"}
		{ani_play "Scouts_Fidget"}
	}	 
	{on "unlink_weapon" sync
		("SC_H_Skin_Weapon_Define_Unlink_Weapon")	
		("SC_H_Request_Define_Unlink_Weapon")
	}	 
	{on fire "hand_right"
		("SC_H_Skin_Weapon_Define_Fire_Hand_Right")	
		("SC_H_Request_Define_Fire_Hand_Right")
	}
	{on blast_hit
		; [...]
	}
	{on bullet_hit
		("Request_Define_Bullet_Hit")
	}
	{on "die"
		("SC_H_Request_Define_Die")
		{View stop "fx_disable_on_death"}	; // <- Turns Randomizer FX Off upon death.
		{View hide "fx_disable_on_death"}	; // <- Turns Randomizer FX Off upon death.
		{if not dead
			{if tagged "skeleton_unit_orkboy" 
				{spawn "orks_spor" "body" x}
			}	
			{if tagged "unit_ork_nob" 
				{spawn "orks_spor" "body" x}
			}
		}
	}
	{on contact	
		("Define_Breed_Tags_Contact")
	}
}

; =============================================================================================================== ;

(Define "Skin_Check_Event_Get_Props"
	{"%0" 
		{on spawn 
			(define "Skin_Select_Attempt"
				{delay %c 
					{if not tagged "tag_skeleton_defined"
						{if prop "inventory_object_Traits"
							{spawn "inventory_item_traits" "body" x 
								{tags add "%0"} 
								{impulse up 0.01}
								{delay 0.1 {delete}}
							}
						else
							{spawn "inventory_item_breeds" "body" x 
								{tags add "%0"} 
								{impulse up 0.01}
								{delay 0.1 {delete}}
							}
						}
					}
				}
			)
			("Skin_Select_Attempt" c(0))
			("Skin_Select_Attempt" c(0.1))
			("Skin_Select_Attempt" c(0.5))
			("Skin_Select_Attempt" c(1.0))
			("Skin_Select_Attempt" c(2.0))
			("Skin_Select_Attempt" c(3.0))
			("Skin_Select_Attempt" c(5.0))
		}
	}
)

("Breed_Skin_Define_Get_Tags" args Get_Props)	; // BUZZ: <- Generetes a prop for each inventory item.
("Trait_Skin_Define_Get_Tags" args Get_Props)	; // BUZZ: <- Generetes a prop for each inventory item.

{"inventory_item"}	; // BUZZ: <- for notice
