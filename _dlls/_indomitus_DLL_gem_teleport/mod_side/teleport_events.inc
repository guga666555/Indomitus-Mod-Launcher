; interaction_entity snippet (human script, launcher-only folder): the visuals of the jump.
; GEM Teleport fires teleport_begin / teleport_jump / teleport_end on the soldier for EVERY {teleport} action.
; To tell abilities apart, each action file starts with {effect "<ability>_start"}, which tags the soldier;
; the shared events then branch on the tag. Each _start removes the other abilities' tags (a teleport cut short
; by death never reaches teleport_end).

{on "jetpack_start"	{tags remove "tp_gate"}		{tags add "tp_jetpack"}}
{on "gate_start"	{tags remove "tp_jetpack"}	{tags add "tp_gate"}}

{on "teleport_begin"				; at the click, unit still at the take-off point, lock starts
	{if tagged "tp_jetpack"
		{play_sound "jetpack_takeoff"}
		{spawn "jetpack_jump" "body"}		; attached to a bone = follows him to the landing point
		;{fsm_request "jetpack-takeoff"}	; take-off animation: play it HERE (it runs inside the lock), length ~ pre_use_delay
	}
	{if tagged "tp_gate"
		;{spawn "warp_flash" "body"}
	}
}
{on "teleport_jump"				; right after the move, unit at the landing point
	{if tagged "tp_jetpack"
		{spawn "jetpack_land" "body"}
		;{fsm_request "jetpack-land"}		; landing animation, length ~ post_use_delay
	}
	{if tagged "tp_gate"
		;{spawn "warp_arrive" "body"}
	}
}
{on "teleport_end"				; release (lock and invulnerability end)
	{if tagged "tp_jetpack" {spawn "jetpack_off"}}
	{tags remove "tp_jetpack"}
	{tags remove "tp_gate"}
}
