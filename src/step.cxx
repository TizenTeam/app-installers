/* 2014, Copyright © Intel Coporation, license MIT, see COPYING file */

#include <alloca.h>
#include <cassert>

#include "step.hxx"

namespace AppInstaller {

FailCode Step::run(const std::vector<Step*> &vector)
{
	struct step *steps, **isteps;
	int i, count, result;

	count = (int)vector.size();
	assert(count >= 0);
	steps = (struct step *)alloca(count * sizeof * steps);
	isteps = (struct step **)alloca(count * sizeof * isteps);
	for (i = 0 ; i < count ; i++) {
		steps[i] = *vector[i];
		isteps[i] = &steps[i];
	}
	return step_run(isteps, count);
}

int Step::_process_(Step *step)
{
	try {
		return step->process();
	}
	catch (const FailException &e) {
		errno = e.saved_errno;
		return e.saved_retcode;
	}
	catch (...) {
		return -1;
	}
}

int Step::_undo_(Step *step)
{
	try {
		return step->undo();
	}
	catch (const FailException &e) {
		errno = e.saved_errno;
		return e.saved_retcode;
	}
	catch (...) {
		return -1;
	}
}

int Step::_clean_(Step *step)
{
	try {
		return step->clean();
	}
	catch (const FailException &e) {
		errno = e.saved_errno;
		return e.saved_retcode;
	}
	catch (...) {
		return -1;
	}
}

}


