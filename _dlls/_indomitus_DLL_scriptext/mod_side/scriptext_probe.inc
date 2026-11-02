;// GEM ScriptExt - probe handlers. Paste inside an entity interaction block of your mod, e.g. your
;// {"human" ...} override (use `overload` if the parent block already defines the same event name).
;// Check the result in gem_scriptext.log (verbose=1: every fired event is logged).
;//
;// The named events are ordinary engine events: {on "move_order"} is exactly what the engine itself
;// would reach with {call "move_order"}. The DLL only performs that call at the right moment.

	{on "move_order"					;// every unit that receives a player move order
		{tags add "sx_move_order"}
		{if "have_enemy"				;// the unit's sensor sees at least one enemy right now (or unquoted: {if have_enemy})
			{tags add "sx_move_under_contact"}
		}
		{delay 3
			{tags remove "sx_move_order"}
			{tags remove "sx_move_under_contact"}
		}
	}

	{on "selection_of_units"			;// every owned unit the player selects
		{tags add "sx_selected"}
		{delay 3
			{tags remove "sx_selected"}
		}
	}

	{on "talk_attack"					;// any other hard-coded talk moment: talk_<name>
		{tags add "sx_attack_order"}
		{delay 3
			{tags remove "sx_attack_order"}
		}
	}

;// {if have_enemy} works anywhere a condition works, in any handler of this block, e.g. an FSM-driven
;// check or a self-rescheduling poll:
;//	{on "sx_poll"
;//		{if "have_enemy"
;//			{tags add "sx_contact"}
;//		else
;//			{tags remove "sx_contact"}
;//		}
;//		{delay 1 {call "sx_poll"}}
;//	}
;//	{on spawn {call "sx_poll"}}
