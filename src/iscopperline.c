/* iscopperline -- exit 0 if running under Copperline, RETURN_WARN otherwise.
 *
 * Detects Copperline's identification board (Zorro manufacturer 5192,
 * product 2 -- the FindConfigDev(5192, 2) convention identify.library
 * uses across the UAE-family emulators; see Copperline's docs/zorro.md).
 * S:User-Startup uses this to tell Copperline apart from Amiberry (or real
 * hardware) sharing the same HardDrives/narrator disk, instead of probing
 * for a driver file that would be installed on the disk either way.
 */

#include <dos/dos.h>
#include <libraries/configvars.h>
#include <libraries/expansionbase.h>
#include <proto/exec.h>
#include <proto/expansion.h>

#define COPPERLINE_MANUFACTURER 5192
#define COPPERLINE_ID_BOARD     2

/* proto/expansion.h declares this extern (struct ExpansionBase *) for the
 * FindConfigDev inline/pragma stub's implicit library-base reference; we
 * provide its one definition here since expansion.library isn't among the
 * bases clib2's startup auto-opens. */
struct ExpansionBase *ExpansionBase;

int main(void)
{
	struct ConfigDev *cd;
	int found;

	ExpansionBase = (struct ExpansionBase *)OpenLibrary((STRPTR)"expansion.library", 36);
	if (!ExpansionBase)
		return RETURN_WARN;

	cd = FindConfigDev(NULL, COPPERLINE_MANUFACTURER, COPPERLINE_ID_BOARD);
	found = (cd != NULL);

	CloseLibrary((struct Library *)ExpansionBase);
	return found ? RETURN_OK : RETURN_WARN;
}
