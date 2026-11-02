;// GEM ScriptExt 0.2 - fauna self-heal. Paste inside the interaction block of your fauna entities
;// (the vehicle defs that carry {props "fauna_main"} and a {health} component), or into a parent
;// block they inherit. Uses the engine's own {delay}+{call} as the tick - the DLL never runs
;// simulation code from its own threads, so the result is identical on every peer.
;//
;// {if "sx_selfheal"} does the whole step when it evaluates:
;//   fauna_main prop + hurt + a "bandage" item in the inventory + (no enemy seen, or the manual key
;//   pressed while the unit is selected)  ->  one bandage removed, +15% max hp, returns true.
;// Otherwise it returns false and changes nothing. Without the DLL the flag is simply never set.

	{on spawn
		{call "sx_heal_poll"}
	}

	{on "sx_heal_poll"
		{if "sx_selfheal"
			{tags add "sx_healed"}			;// optional: drive a sound / fx / talk from this tag
			{delay 1 {tags remove "sx_healed"}}
		}
		{delay 1 {call "sx_heal_poll"}}		;// 1 s poll: the manual key window (ini manual_heal_window_ms) must be longer than this
	}

;// {if "sx_fauna"} is true on any entity whose def carries the fauna prop - handy to branch shared
;// human/fauna scripts:
;//	{on "move_order"
;//		{if "sx_fauna" {tags add "sx_fauna_moving"} }
;//	}
