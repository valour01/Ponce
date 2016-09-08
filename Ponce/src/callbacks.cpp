#include <list>
// Ponce
#include "callbacks.hpp"
#include "globals.hpp"
#include "context.hpp"
#include "utils.hpp"
#include "tainting_n_symbolic.hpp"

//IDA
#include <ida.hpp>
#include <dbg.hpp>
#include <loader.hpp>
#include <intel.hpp>

//Triton
#include "api.hpp"
#include "x86Specifications.hpp"

std::list<breakpoint_pending_action> breakpoint_pending_actions;

/*This function will create and fill the Triton object for every instruction*/
void tritonize(ea_t pc, thid_t threadID)
{
	/*Check tha the runtime Trigger is on just in case*/
	if (!runtimeTrigger.getState())
		return;

	//We delete the last_instruction
	//Maybe in the future we need to keep the in instruction memory to negate the condition at any moment
	if (last_triton_instruction != NULL)
		delete last_triton_instruction;
	triton::arch::Instruction* tritonInst = new triton::arch::Instruction();
	last_triton_instruction = tritonInst;
	//ea_t pc = va_arg(va, ea_t);
	/*This will fill the 'cmd' (to get the instruction size) which is a insn_t structure https://www.hex-rays.com/products/ida/support/sdkdoc/classinsn__t.html */
	if (!decode_insn(pc))
		warning("[!] Some error decoding instruction at %p", pc);	
	
	//thid_t threadID = va_arg(va, thid_t);
	/*char buf[50];
	ua_mnem(pc, buf, sizeof(buf));*/
	unsigned char opcodes[15];
	get_many_bytes(pc, opcodes, sizeof(opcodes));

	/* Setup Triton information */
	tritonInst->partialReset();
	tritonInst->setOpcodes((triton::uint8*)opcodes, cmd.size);
	tritonInst->setAddress(pc);
	tritonInst->setThreadId(threadID);

	/* Disassemble the instruction */
	try{
		triton::api.disassembly(*tritonInst);
	}
	catch (...){
		msg("[!] Dissasembling error at "HEX_FORMAT" Opcodes:",pc);
		for (auto i = 0; i < cmd.size; i++)
			msg("%2x ", *(unsigned char*)(opcodes + i));
		msg("\n");
		return;
	}
	if (cmdOptions.showDebugInfo)
		msg("[+] Triton At "HEX_FORMAT": %s (Thread id: %d)\n", pc, tritonInst->getDisassembly().c_str(), threadID);

	/*std::list<triton::arch::OperandWrapper> tainted_reg_operands;
	if (ADD_COMMENTS_WITH_TAINTING_INFORMATION)
		tainted_reg_operands = get_tainted_regs_operands(tritonInst);*/

	/* Process the IR and taint */
	triton::api.buildSemantics(*tritonInst);

	if (cmdOptions.addCommentsControlledOperands)
		get_controlled_operands_and_add_comment(tritonInst, pc);// , tainted_reg_operands);

	if (cmdOptions.addCommentsSymbolicExpresions)
		add_symbolic_expressions(tritonInst, pc);

	/* Trust operands */
	for (auto op = tritonInst->operands.begin(); op != tritonInst->operands.end(); op++)
		op->setTrust(true);

	//ToDo: The isSymbolized is missidentifying like "user-controlled" some instructions: https://github.com/JonathanSalwan/Triton/issues/383
	if (tritonInst->isTainted() || tritonInst->isSymbolized())
	{
		if (cmdOptions.showDebugInfo)
			msg("[!] Instruction %s at "HEX_FORMAT"\n", tritonInst->isTainted()? "tainted": "symbolized", pc);
		if (cmdOptions.RenameTaintedFunctionNames)
			rename_tainted_function(pc);
		if (tritonInst->isBranch()) // Check if it is a conditional jump
			set_item_color(pc, cmdOptions.color_tainted_condition);
		else
			set_item_color(pc, cmdOptions.color_tainted);
	}

	if (tritonInst->isBranch() && tritonInst->isSymbolized())
	{
		triton::__uint addr1 = (triton::__uint)tritonInst->getNextAddress();
		triton::__uint addr2 = (triton::__uint)tritonInst->operands[0].getImmediate().getValue();
		if (cmdOptions.showDebugInfo)
			msg("[+] Branch symbolized detected at "HEX_FORMAT": "HEX_FORMAT" or "HEX_FORMAT", Taken:%s\n", pc, addr1, addr2, tritonInst->isConditionTaken() ? "Yes" : "No");
		triton::__uint ripId = triton::api.getSymbolicRegisterId(TRITON_X86_REG_PC);
		if (tritonInst->isConditionTaken())
			myPathConstraints.push_back(PathConstraint(ripId, pc, addr2, addr1));
		else
			myPathConstraints.push_back(PathConstraint(ripId, pc, addr1, addr2));
	}
	//We add the instruction to the map, so we can use it later to negate conditions, view SE, slicing, etc..
	//instructions_executed_map[pc].push_back(tritonInst);
}

/*This function is called when we taint a register that is used in the current instruction*/
void reanalize_current_instruction()
{
	if (cmdOptions.showDebugInfo)
		msg("Reanalizyng instruction at "HEX_FORMAT"\n");
	uint64 eip;
	get_reg_val("eip", &eip);
	tritonize((triton::__uint)eip, get_current_thread());
}

/*This functions is called every time a new debugger session starts*/
void triton_restart_engines()
{
	if (cmdOptions.showDebugInfo)
		msg("[+] Restarting triton engines...\n");
	//We reset everything at the beginning
	triton::api.resetEngines();
	//If we are in taint analysis mode we enable only the tainting engine and disable the symbolic one
	triton::api.getTaintEngine()->enable(cmdOptions.use_tainting_engine);
	triton::api.getSymbolicEngine()->enable(cmdOptions.use_symbolic_engine);
	//triton::api.getSymbolicEngine()->enable(true);
	runtimeTrigger.disable();
	is_something_tainted_or_symbolize = false;
	tainted_functions_index = 0;
	//Reset instruction counter
	total_number_traced_ins = current_trace_counter = 0;
	breakpoint_pending_actions.clear();
	set_automatic_taint_n_simbolic();
	myPathConstraints.clear();
}

int idaapi tracer_callback(void *user_data, int notification_code, va_list va)
{
	//msg("Notification code: %d str: %s\n",notification_code, notification_code_to_string(notification_code).c_str());
	switch (notification_code)
	{
		case dbg_process_start:
		{
			if (cmdOptions.showDebugInfo)
				msg("[+] Starting the debugged process. Reseting all the engines.\n");
			triton_restart_engines();
			clear_requests_queue();
			break;
		}
		case dbg_step_into:
		case dbg_step_over:
		{
			//msg("dbg_step_?\n");
			//If tracing is enable for each one of this event is launched another dbg_trace. So we should ignore this one
			/*if (ENABLE_TRACING_WHEN_TAINTING)
				break;*/
			//We want to enable the user to do step into/over, so he could choose whitch functions skip and with conditions negate
			debug_event_t* debug_event = va_arg(va, debug_event_t*);
			thid_t tid = debug_event->tid;
			ea_t pc = debug_event->ea;
			//msg("dbg_step_? at "HEX_FORMAT"\n", pc);
			//We need to check if the instruction has been analyzed already. This happens when we are stepping into/over and 
			//we find a breakpoint we set (main, recv, fread), we are receiving two events: dbg_bpt and dbg_step_into for the 
			//same instruction. And we want to tritonize in dbg_bpt for example when we put bp in main and we execute the program
			if (last_triton_instruction != NULL && last_triton_instruction->getAddress() != pc)
			{
				if (cmdOptions.showExtraDebugInfo)
					msg("[+] Stepping %s: "HEX_FORMAT" (Tid: %d)\n", notification_code == dbg_step_into ? "into" : "over", pc, tid);
				if (cmdOptions.paintExecutedInstructions)
					set_item_color(pc, cmdOptions.color_executed_instruction);
				tritonize(pc, tid);
			}
			/*else
			{
				if (last_triton_instruction == NULL)
					msg("last_triton)isntructionn NULL\n");
				else
					msg("last_triton_instruction->getAddress(): "HEX_FORMAT"\n", last_triton_instruction->getAddress());
			}*/
			//Continue stepping
			//msg("automatically_continue_after_step: %d\n", automatically_continue_after_step);
			if (automatically_continue_after_step)
			{
				//This is the wow64 switching, we need to skip it
				if (last_triton_instruction->getDisassembly().find("call dword ptr fs:[0xc0]") != -1)
				{
					msg("wow64 switching! request_step_over();\n");
					request_step_over();
				}
				else// if (notification_code == dbg_step_into)
				{
					msg("dbg_step request_step_into();\n");
					request_step_into();
				}
				/*else
				{
					msg("dbg_step request_step_over();\n");
					request_step_over();
				}*/
			}
			break;
		}
		case dbg_trace:
		{
			break;
			// A step occured (one instruction was executed). This event
			// notification is only generated if step tracing is enabled.
			//msg("dbg_trace\n");
			//Create the triton instance for the Instruction

			thid_t tid = va_arg(va, thid_t);
			ea_t pc = va_arg(va, ea_t);

			//msg("[%d] tracing over: "HEX_FORMAT"\n", g_nb_insn, pc);
			if (cmdOptions.paintExecutedInstructions)
				set_item_color(pc, cmdOptions.color_executed_instruction);
			tritonize(pc, tid);

			current_trace_counter++;
			total_number_traced_ins++;

			if (cmdOptions.limitInstructionsTracingMode && current_trace_counter == cmdOptions.limitInstructionsTracingMode)
			{
				int answer = askyn_c(1, "[?] %d instructions has been traced. Do you want to execute %d more?", total_number_traced_ins, cmdOptions.limitInstructionsTracingMode);
				if (answer == 0 || answer == -1) //No or Cancel
				{
					// stop the trace mode and suspend the process
					disable_step_trace();
					suspend_process();
					msg("[!] Process suspended (Traced %d instructions)\n", total_number_traced_ins);
				}
				else
				{
					current_trace_counter = 0;
				}
			}
			break;
		}
		case dbg_bpt:
		{
			thid_t tid = va_arg(va, thid_t);
			ea_t pc = va_arg(va, ea_t);
			int *warn = va_arg(va, int *);
			//msg("dbg_bpt at "HEX_FORMAT"\n", pc);
			//This variable defines if a breakpoint is a user-defined breakpoint or not
			bool user_bp = true;
			//msg("Breakpoint reached! At "HEX_FORMAT"\n", pc);
			//We look if there is a pending action for this breakpoint
			for (auto it = breakpoint_pending_actions.begin(); it != breakpoint_pending_actions.end(); ++it)
			{
				breakpoint_pending_action bpa = *it;
				//If we find a pendign action we execute the callback
				if (pc == bpa.address)
				{
					bpa.callback(pc);
					if (cmdOptions.paintExecutedInstructions)
						set_item_color(pc, cmdOptions.color_executed_instruction);
					tritonize(pc, tid);
					//If there is a user-defined bp in the same address we should respect it and dont continue the exec
					if (!bpa.ignore_breakpoint)
					{
						//If it a breakpoint the plugin set not a user-defined bp
						user_bp = false;
						//If not this is the bp we set to taint the arguments, we should rmeove it and continue the execution
						del_bpt(pc);
						//msg("after bp automatically_continue_after_step: %d\n", automatically_continue_after_step);
						if (ENABLE_STEP_INTO_WHEN_TAINTING)// && automatically_continue_after_step)
						{
							automatically_continue_after_step = true;
							msg("after bp request_step_into\n");
							request_step_into();
							run_requests();
						}
						else
							continue_process();
					}
					break;
				}
			}
			//If it is a user-defined bp we disable the automatic stepping
			if (user_bp)
				automatically_continue_after_step = false;
			break;
		}
		case dbg_process_exit:
		{
			if (cmdOptions.showDebugInfo)
				msg("[!] Process_exiting...\n");
			/*if (ENABLE_TRACING_WHEN_TAINTING)
			{
				if (DEBUG)
					msg("[+] Clearing trace...\n");
				clear_trace();
			}*/
			//msg("In dbg_process_exit, reseting everything\n");
			//Do we want to unhook this event?
			//unhook_from_notification_point(HT_DBG, tracer_callback, NULL);
			runtimeTrigger.disable();
			break;
		}
	}
	return 0;
}

//---------------------------------------------------------------------------
// Callback for ui notifications
int idaapi ui_callback(void * ud, int notification_code, va_list va)
{
	switch (notification_code)
	{
		// called when IDA is preparing a context menu for a view
		// Here dynamic context-depending user menu items can be added.
		case ui_populating_tform_popup:
		{
			TForm *form = va_arg(va, TForm *);
			TPopupMenu *popup_handle = va_arg(va, TPopupMenu *);
			int view_type= get_tform_type(form);

			
			/*Iterate over all the actions*/
			for (int i = 0;; i++)
			{
				if (action_list[i].action_decs == NULL)
					break;
			
				/*Iterate over the view types of every action*/
				for (int j=0;; j++)
				{
					if (action_list[i].view_type[j] == NULL){
						break;
					}
					if (action_list[i].view_type[j] == view_type)
					{
						//We only attach to the popup if the action makes sense with the current configuration
						if (cmdOptions.use_tainting_engine && action_list[i].taint || cmdOptions.use_symbolic_engine && action_list[i].symbolic)
						{
							attach_action_to_popup(form, popup_handle, action_list[i].action_decs->name, NULL, SETMENU_FIRST);
							
						}
						//To disable an action
						//enable_menu_item(action_list[i].action_decs->name, false);
					}
				}	
			}
		}
		case dbg_process_exit:{
			unhook_from_notification_point(HT_DBG, ui_callback, NULL);
			break;
		}
	}
	return 0;
}

