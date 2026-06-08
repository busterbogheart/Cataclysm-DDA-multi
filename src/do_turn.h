#pragma once
#ifndef CATA_SRC_DO_TURN_H
#define CATA_SRC_DO_TURN_H

// timeout: ms to block waiting for an input event. 0 = non-blocking poll (SP /
// client default). The MP host wait passes a small timeout (e.g. 16ms) so keys are
// caught reliably — its SRV-WAIT sub-loop doesn't pump SDL like the main loop does.
void handle_key_blocking_activity( int timeout = 0 );

#endif // CATA_SRC_DO_TURN_H
