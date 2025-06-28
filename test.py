import time
import threading
from pynput import keyboard

# --- Configuration ---
TOGGLE_KEY = keyboard.Key.f9  # Key to start/stop the autoclicker
EXIT_KEY = keyboard.Key.esc  # Key to exit the script
KEY_SEQUENCE = ["1", "1", "s", "s"]  # Keys to press in sequence
DELAY_AFTER_SEQUENCE = 0.03  # Delay in seconds after each sequence

# --- Global Variables ---
autoclicker_active = False  # State of the autoclicker (on/off)
worker_thread = None  # Thread for the clicking action
stop_event = threading.Event()  # Event to signal the worker thread to stop

# Initialize the keyboard controller
keyboard_controller = keyboard.Controller()


def autoclicker_worker():
    """
    This function runs in a separate thread and performs the key presses
    as long as autoclicker_active is True and stop_event is not set.
    """
    global autoclicker_active
    print("Autoclicker sequence started...")
    while autoclicker_active and not stop_event.is_set():
        try:
            keyboard_controller.press("1")
            keyboard_controller.press("s")
            keyboard_controller.release("1")
            time.sleep(DELAY_AFTER_SEQUENCE)  # Wait after pressing the keys
            keyboard_controller.release("s")
            time.sleep(DELAY_AFTER_SEQUENCE + 0.01)  # Wait before next sequence
        except Exception as e:
            print(f"Error during clicking sequence: {e}")
            # Optionally stop the autoclicker on error
            # autoclicker_active = False
            # stop_event.set()
            break  # Exit loop on error

    if not stop_event.is_set():  # If stopped by toggling off, not by exit key
        print("Autoclicker sequence stopped.")
    stop_event.clear()  # Reset event for next activation


def on_press(key):
    """
    Callback function for key presses.
    Toggles the autoclicker on/off with F9.
    Exits the script with ESC.
    """
    global autoclicker_active, worker_thread

    try:
        if key == TOGGLE_KEY:
            if autoclicker_active:
                print("F9 pressed: Stopping autoclicker...")
                autoclicker_active = False
                stop_event.set()  # Signal the worker thread to stop
                if worker_thread and worker_thread.is_alive():
                    worker_thread.join()  # Wait for the thread to finish
                print("Autoclicker is now OFF.")
            else:
                print("F9 pressed: Starting autoclicker...")
                autoclicker_active = True
                stop_event.clear()  # Clear the stop event before starting
                # Create and start a new worker thread
                worker_thread = threading.Thread(target=autoclicker_worker)
                worker_thread.daemon = (
                    True  # Allow main program to exit even if thread is running
                )
                worker_thread.start()
                print(
                    f"Autoclicker is now ON. Press F9 to stop, or ESC to exit program."
                )

        elif key == EXIT_KEY:
            print("ESC pressed: Exiting program...")
            if autoclicker_active:
                autoclicker_active = False
                stop_event.set()
                if worker_thread and worker_thread.is_alive():
                    worker_thread.join()
            return False  # Stop the listener

    except Exception as e:
        print(f"Error in on_press: {e}")
        return False  # Stop listener on error to be safe


def main():
    """Main function to start the keyboard listener."""
    print(f"--- Python Autoclicker ---")
    print(f"Press {TOGGLE_KEY} to start/stop the autoclicker.")
    print(
        f"The sequence is: press '1', press 's', wait {DELAY_AFTER_SEQUENCE}s, repeat."
    )
    print(f"Press {EXIT_KEY} to exit the script entirely.")
    print("--------------------------")
    print("Listening for key presses...")

    # Collect events until released
    with keyboard.Listener(on_press=on_press) as listener:
        listener.join()

    print("Script finished.")


if __name__ == "__main__":
    main()
