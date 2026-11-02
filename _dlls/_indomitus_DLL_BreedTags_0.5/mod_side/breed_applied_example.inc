; GEM BreedTags 0.6 - example only, paste into your human interaction block (next to the existing {on spawn}).
; {on "breed_applied"} fires after a breed has been applied to a unit that is already in the scene (a re-breed in
; the conquest/army preview, or a breed the engine applied on the unit's first update). The breed's tags are already
; on the unit at that point, and the previous breed's tags have been removed.
	{on spawn
		{Delay 0.01 ("Skin_Prop_Selector")}
	}
	{on "breed_applied"
		{Delay 0.01 ("Skin_Prop_Selector")}
	}
