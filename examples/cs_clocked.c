#include <stdio.h>
#include <stdbool.h>
#include <windows.h>
#include <strsafe.h>

#include <process.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <assert.h>
#include "fmi3Functions.h"
#include "util.h"
#include "config.h"

/* *****************  Forward declarations ***************** */
fmi3Status initializeOutputFiles();
unsigned __stdcall thr_activateModelPartition(void *args);
static bool setAndCheckInputClocks(fmi3Instance s, fmi3Float64 time);
static bool checkOutputClocks(fmi3Instance s);
fmi3Status cb_intermediateUpdate(fmi3Instance s, fmi3IntermediateUpdateInfo* intermediateUpdateInfo);
fmi3Status cb_lockPreemption(fmi3Instance s); 
fmi3Status cb_unlockPreemption(fmi3Instance s); 
fmi3Status setDebugLogging(fmi3Instance* comp, bool loggingOn, size_t nCategories, const char* const categories[]);
void logEvent(fmi3Instance* comp, const char* message, ...);
/* ********************************************************* */


#define CHECK_STATUS(S) status = S; if (status != fmi3OK) goto out;

/* *****************  Global variables ***************** */
// Global lock for protecting the access to the model's memory area
extern HGLOBAL globalLockVar;

// All threads for the model partitions are supposed to run on the same processor
// stores the to-be-used processor for the thread creation.
DWORD_PTR processorMask;

// current simulation time
fmi3Float64 time = START_TIME;

// Simulation stop time
const fmi3Float64 stopTime = STOP_TIME;  

// Communication step size
fmi3Float64 stepSize = FIXED_SOLVER_STEP;  // => 1

// global arguments for intermediate update operation
ThreadArgs iu_arguments;
/* ********************************************************* */

/* *****************  Inputs ***************** */
fmi3Int32 inputs_c2[1] = { 0 };
const fmi3ValueReference vrInputs_c2[1] = { vr_input_2 };

/* ********************************************************* */

/* *****************  Outputs ***************** */
// separate the output variables after their clock (== modelpartition)
fmi3Int32 outputs_c1[3] = { 0 };
fmi3Int32 outputs_c2[2] = { 0 };
fmi3Int32 outputs_c3[2] = { 0 };
const fmi3ValueReference vrOutputs_c1[2] = { vr_InClock_1_Ticks, vr_total_InClock_Ticks};
const fmi3ValueReference vrOutputs_c2[2] = { vr_InClock_2_Ticks , vr_result_2 };
const fmi3ValueReference vrOutputs_c3[2] = { vr_InClock_3_Ticks, vr_output_3 };

/* ********************************************************* */

/* *****************  Input clocks ***************** */
fmi3Clock inputClocks[N_INPUT_CLOCKS] = { fmi3ClockInactive };
const fmi3ValueReference vrInputClocks[N_INPUT_CLOCKS] = { vr_InClock_1, vr_InClock_2, vr_InClock_3 };

/*
 * For FMI3.0 lower priority value  means higher priority
 * In windows lower value means lower prio 
 * In windows we only have 5 prio levels [-2 .. +2]
 *
 * Setting the values based on the clock priorities from the ModelDescription.xml
 */
fmi3Int32 inputClockPrio[N_INPUT_CLOCKS] = { 2, 1, -1 };
/* ********************************************************* */

/* *****************  Output clocks ***************** */
fmi3Clock outputClocks[N_OUTPUT_CLOCKS] = { fmi3ClockInactive };
const fmi3ValueReference vrOutputClocks[N_OUTPUT_CLOCKS] = { vr_OutClock_1, vr_OutClock_2 };
/* ********************************************************* */

/* *****************  Misc ***************** */
// Results are printed into these files
FILE *outputFile[N_INPUT_CLOCKS];
/* ********************************************************* */


int main(int argc, char* argv[]) {

	fmi3Status status = fmi3OK;
	fmi3Status terminateStatus = fmi3OK;
	unsigned int returnval[N_INPUT_CLOCKS]; // return values of all possible threads

	const fmi3CallbackFunctions callbacks = {
		.instanceEnvironment = NULL,
		.logMessage = cb_logMessage,
		.allocateMemory = cb_allocateMemory,
		.freeMemory = cb_freeMemory,
		.intermediateUpdate = cb_intermediateUpdate,
		.lockPreemption = cb_lockPreemption,
		.unlockPreemption = cb_unlockPreemption
	};
	
	//set Co-Simulation mode
	const fmi3CoSimulationConfiguration csConfig = {
		.intermediateVariableGetRequired = fmi3False,
		.intermediateInternalVariableGetRequired = fmi3False,
		.intermediateVariableSetRequired = fmi3False,
	};

	// Instantiate slave
	const fmi3Instance s = fmi3Instantiate("instance", fmi3CoSimulation, MODEL_GUID, "", &callbacks, fmi3False, fmi3True, &csConfig);

	if (s == NULL) {
		status = fmi3Error;
		goto out;
	}

	// Initialize logging
	char* const categories[1] = { "logEvents" };
	setDebugLogging(s, true, 1, categories);
	logEvent(s, "Running Scheduled Execution Co-Simulation example...\n");

	// Get a global lock
	globalLockVar = GlobalAlloc(GMEM_FIXED, sizeof(globalLockVar));
	if (globalLockVar == NULL) {
		status = fmi3Error;
		logEvent(s, "Allocating the global lock failed: %ld", GetLastError());
		goto out;
	}

	CHECK_STATUS(initializeOutputFiles());

	// Initialize slave
	CHECK_STATUS(fmi3SetupExperiment(s, fmi3False, 0.0, time, fmi3True, stopTime));
	CHECK_STATUS(fmi3EnterInitializationMode(s));

	// update clocks
	CHECK_STATUS(fmi3GetClock(s, vrOutputClocks, N_OUTPUT_CLOCKS, outputClocks));

	/* 
	 * Thread related stuff below 
	 */
	// Handles to access threads after they have been started. One for each input clock
	HANDLE thrHandle[N_INPUT_CLOCKS];
	// Need a set of arguments for every Inputclock. otherwise the arguments will be overwritten
	ThreadArgs thrArguments[N_INPUT_CLOCKS]; 
	int curThrHandle;

	DWORD_PTR processorNumber = GetCurrentProcessorNumber();
	processorMask = (((DWORD_PTR)1 )<< processorNumber);
	logEvent(s, "GetCurrentProcessorNumber() => %lx (Mask = %lx)", processorNumber, processorMask);

	while (time < stopTime) {

		// According to the current time, the non-dependent input clocks are set
		// the call below returns true, if at least one of the clocks is ticking
		if (setAndCheckInputClocks(s, time)) {
			logEvent(s, "==========> time =%g", time);

			curThrHandle = 0; // Number of threads that have been fired at this particular point in time
			for (int i = 0; i < N_INPUT_CLOCKS; i++)
			{
				if (inputClocks[i] == fmi3ClockActive) {
					logEvent(s, "starting thread for clock %s (vr=%d)", i == InClock_1 ? "InClock_1" : "InClock_2", vrInputClocks[i]);
					thrArguments[i].comp = s;
					thrArguments[i].clockRef = vrInputClocks[i];
					thrArguments[i].retval = &returnval[i];
					thrArguments[i].activationTime = time;

					thrHandle[curThrHandle] = (HANDLE)_beginthreadex(NULL, 0, thr_activateModelPartition, &thrArguments[i], 0, NULL);
					if (thrHandle[curThrHandle] != NULL) {
						SetThreadAffinityMask(thrHandle[curThrHandle], processorMask);
						SetThreadPriority(thrHandle[curThrHandle], inputClockPrio[i]);
					}
					else {
						logEvent(s, "Could not create thread in main loop");
						status = fmi3Fatal;
						goto out;
					}
					curThrHandle++;
					inputClocks[i] = fmi3ClockInactive;
				}
			}
			// All needed threads are started, now let the time move forward
		}
		Sleep(950); // sleep for a little less than step time (???)
		time += stepSize;
	}

	
out:
	

	if (s && status != fmi3Error && status != fmi3Fatal) {
		terminateStatus = fmi3Terminate(s);
	}

	GlobalFree(globalLockVar);

	for (int part = 0; part < N_INPUT_CLOCKS; part++) {
		fclose(outputFile[part]);
	}

	logEvent(s, "... finished Scheduled Execution Co-Simulation example.\n");
	if (s && status != fmi3Fatal && terminateStatus != fmi3Fatal) {
		fmi3FreeInstance(s); // After this point, logEvent is no longer possible
	}
	return status == fmi3OK ? EXIT_SUCCESS : EXIT_FAILURE;

}

/*
* initializeOutputFiles()
*/
fmi3Status initializeOutputFiles() {
	char outfile[32];
	for (int part = 0; part < N_INPUT_CLOCKS; part++)
	{
		sprintf(outfile, "Scheduled_part_%d_out.csv", part + 1);
		outputFile[part] = fopen(outfile, "w");
		if (!outputFile[part]) {
			puts("Failed to open output file.");
			return fmi3Error;
		}
	}
	// write the headers of the CSVs  (we have knowledge about how many partitions we actually have)
	fputs("time,InClock_1_Ticks,total_InClock_Ticks\n", outputFile[0]);
	fputs("time,InClock_2_Ticks,result\n", outputFile[1]); 
	fputs("time,InClock_3_Ticks,output_3\n", outputFile[2]); 
	return fmi3OK;
}

/*
 * recordVariables(s, time, modelPart)
 * writes the current values of the variables associated to modelPart into the outputFile
 * The status of the all clocks are always printed
 * returns fmi3OK on success
 * returns fmi3Error on failure 
 */
fmi3Status recordVariables(fmi3Instance s, fmi3Float64 time, int modelPart) {

	if (modelPart == vr_InClock_1) {
		fprintf(outputFile[0], "%g,%3d,%3d\n", time, outputs_c1[0], outputs_c1[1]);
		logEvent(s, "%g,%3d,%3d", time, outputs_c1[0], outputs_c1[1]);
	}
	else if (modelPart == vr_InClock_2) {
		fprintf(outputFile[1], "%g,%3d,%3d\n", time, outputs_c2[0], outputs_c2[1]);
		logEvent(s, "%g,%3d,%3d", time, outputs_c2[0], outputs_c2[1]);
	}
	else if (modelPart == vr_InClock_3) {
		fprintf(outputFile[2], "%g,%3d,%3d\n", time, outputs_c3[0], outputs_c3[1]);
		logEvent(s, "%g,%3d,%3d", time, outputs_c3[0], outputs_c3[1]);
	}
	return fmi3OK;
}

/*
 * cb_intermediateUpdate()
 *
 * callback function, initiated when a modelpartition calculates the ticking of an output clock
 * This function checks, if any ticking output clock is connected to a dependent input clock.
 * If so, the thread for the respective model partition is started on the designated processor and with the designated priority.
 *
 * returns always fmi3OK (practically a void funtion)
 */
fmi3Status cb_intermediateUpdate(fmi3Instance s, fmi3IntermediateUpdateInfo* intermediateUpdateInfo) {
	HANDLE thr_handle;
	int returnval;

	// In this example we only check for ticking output clocks.
	// If there is no such clock, that's ok.
	if (!intermediateUpdateInfo->clocksTicked || !checkOutputClocks(s)) {
		logEvent(s, "No clock active in intermediateUpdate callback");
		return fmi3OK;
	}

	// Some output clock ticked, check for dependend input clocks and, if there are any, fire them
	// input clock 3 depends on output clock 1
	// This dependency is taken from ModelDescription.xml 
	if (outputClocks[OutClock_1]) {
		// create a thread that will run the model partition for input clock 3
		logEvent(s, "cb_intermediateUpdate starting thread for input clock 3 (vr=%d)", vr_InClock_3);

		iu_arguments.comp = s;
		iu_arguments.clockRef = vr_InClock_3;
		iu_arguments.retval = &returnval;
		iu_arguments.activationTime = time;
		thr_handle = (HANDLE)_beginthreadex(NULL, 0, thr_activateModelPartition, &iu_arguments, 0, NULL);
		if (thr_handle != NULL) {
			// Bind the thread to the designated processor
			SetThreadAffinityMask(thr_handle, processorMask);
			// Set the designated priority
			SetThreadPriority(thr_handle, inputClockPrio[InClock_3]);
		}
		else {
			logEvent(s, "Could not create thread in cb_intermediateUpdate");
		}

	}
	if (outputClocks[OutClock_2]) {
		//  so far, no other action but a log message
		logEvent(s, "Detected ticking of output clock 2 (time=%g)", time);
	}
	return fmi3OK;

}
/*
 * cb_lockPreemption()
 * Callback function to grab a lock in order to avoid preemption in a critical section
 * Works on a globally defined variable initialized by the master
*/
fmi3Status cb_lockPreemption(fmi3Instance s) {
	return (GlobalLock(globalLockVar) == NULL)? fmi3Error : fmi3OK;
}
/*
 * cb_unlockPreemption()
 * Callback function to release a lock 
 * Works on a globally defined variable initialized by the master
*/
fmi3Status cb_unlockPreemption(fmi3Instance s) {
	return GlobalUnlock(globalLockVar);
}


/*
 * setAndCheckInputClocks()
 *
 * Function to set and determine the state of all *independent* Input clocks. 
 * This depends on the current time (within the simulation) only
 * returns true, if any of the independent input clocks are set and thus a model partition must be activated
 */
static bool setAndCheckInputClocks(fmi3Instance s, fmi3Float64 time) {
	// InClock_1: active every second
	if ((int)time % 1 == 0) { 
		inputClocks[InClock_1] = fmi3ClockActive;
	}
	else {
		inputClocks[InClock_1] = fmi3ClockInactive;
	}
	// InClock_2: active at 0, 1, 8 and 9
	if (((int)time % 8 == 0) || ((int)time - 1) % 8 == 0) {
		inputClocks[InClock_2] = fmi3ClockActive;
	}
	else {
		inputClocks[InClock_2] = fmi3ClockInactive;
	}

	bool retval = inputClocks[InClock_1] == fmi3ClockActive || inputClocks[InClock_2] == fmi3ClockActive;
	if (retval) {
		logEvent(s, "setAndCheckInputClocks: time=%d, inputClocks[InClock_1] = %d inputClocks[InClock_2] = %d",
			(int)time, inputClocks[InClock_1], inputClocks[InClock_2]);
	}
	return retval;
}

/*
 * This function retrieves the state of the output clocks of all partitions of the slave
 * outputClocks[] is set accordingly
 * returns true if any of the outputClocks is actually set
 */
static bool checkOutputClocks(fmi3Instance s) {
	int cl;
	for (cl=0; cl< N_OUTPUT_CLOCKS; cl++) 
		outputClocks[cl] = fmi3ClockInactive;

    fmi3GetClock(s, vrOutputClocks, N_OUTPUT_CLOCKS, outputClocks);
    return outputClocks[OutClock_1] || outputClocks[OutClock_2];
}

/*
 * thr_activateModelPartition(args)
 *
 * called within a thread creation, whenever a model-partition is due to run
 * argument is an opaque pointer pointing to the necessary thread arguments => comp, clockRef, activationTime
 *
 * 1. the inputs of the respective model partition are set
 * 2. framework function fmi3ActivateModelPartition is called
 * 3. all outputs of the respective model partition are retrieved 
 * 4. the outputs are recorded 
 * 5. finally, the thread is ended, returning  fmi3Status (fmi3OK or fmi3Error)
 */
unsigned __stdcall thr_activateModelPartition(void *args)  {
	ThreadArgs* TA = args;
	fmi3Status retval = fmi3OK;

	// Set Variables of appropriate partition
	switch (TA->clockRef) {
		case vr_InClock_1: {
			logEvent(TA->comp, "activateModelPartition calling fmi3ActivateModelPartition (%d)", TA->clockRef);
			// No variables to set for this partition
			retval = fmi3ActivateModelPartition(TA->comp, TA->clockRef, TA->activationTime);
			if (retval != fmi3OK)	break;
			retval = fmi3GetInt32(TA->comp, vrOutputs_c1, 2, &outputs_c1[0], 2);
			recordVariables(TA->comp, TA->activationTime, vr_InClock_1);
		}
		break;
		case vr_InClock_2: {
			logEvent(TA->comp, "activateModelPartition calling fmi3ActivateModelPartition (%d)", TA->clockRef);
			retval = fmi3SetInt32(TA->comp, vrInputs_c2, 1, inputs_c2, 1);
			// Reset the source for the input again, so it is counted just once 
			inputs_c2[0] = 0;
			if (retval != fmi3OK) break;
			retval = fmi3ActivateModelPartition(TA->comp, TA->clockRef, TA->activationTime);
			if (retval != fmi3OK)	break;
			retval = fmi3GetInt32(TA->comp, vrOutputs_c2, 2, outputs_c2, 2);
			recordVariables(TA->comp, TA->activationTime, vr_InClock_2);
		}
		break;
		case vr_InClock_3: {
			logEvent(TA->comp, "activateModelPartition calling fmi3ActivateModelPartition (%d)", TA->clockRef);
			// No variables to set for this partition
			if (retval != fmi3OK)	break;
			retval = fmi3ActivateModelPartition(TA->comp, TA->clockRef, TA->activationTime);
			if (retval != fmi3OK)	break;
			retval = fmi3GetInt32(TA->comp, vrOutputs_c3, 2, outputs_c3, 2);
			// Use the output of model part 3 as input for model part 2
			inputs_c2[0] = outputs_c3[1];
			recordVariables(TA->comp, TA->activationTime, vr_InClock_3);
		}
		break;
		default:
			retval = fmi3Error;
	}
	_endthreadex(retval);
	return 0;
}