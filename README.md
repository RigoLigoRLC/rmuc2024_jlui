# JLUI

[让我们说中文](README_CN.md)

Client side UI library for RoboMaster University Series Embedded Devs, by Team TARS_Go from Jilin University.

This library has seen extensive use in RoboMaster University Championship 2024 in our team and also some other teams from nearby universities. But the quality of this library will still be ABSOLUTELY NO WARRANTY as described in the [license](LICENSE).

This library is distributed under MIT license.

# Porting

## Implementing Interfaces
Users should implement the following functions, they will be called by JLUI, make sure JLUI can link against them:

```c
int JLUI_MutexLock(void *mutex);
void JLUI_MutexUnlock(void *mutex);
void JLUI_SendData(const uint8_t *data, size_t len);
```

Mutex is used to protect the internal states in case multiple threads try updating graphics elements. This means you should refrain from calling JLUI APIs when in an ISR -- beware of hanging the system and breaking the real time system.

Your `JLUI_SendData` implementation should send specified amount of bytes to the serial port connected to Referee System serial port. Notice that JLUI assumes this function blocks execution until the transmission is done, and the memory pointed by `data` pointer should be considered invalid once the function returns. If you need asynchronous transmission, please copy the data buffer.

## Calling APIs

### Initialization

Call this function at startup before calling any other JLUI APIs:

```c
void JLUI_SetSenderReceiverId(uint16_t senderId, uint16_t receiverId);
```

Set your own Robot ID and Player Client ID right here. This also clears internal buffer.

Set a mutex object with this API call. If you intend to not use mutex just set it to an arbitrary non-zero pointer.

```c
void JLUI_SetMutexObject(void *mutex);
```

### Periodic sending

As of the time of writing (Serial Port Protocol Appendix V1.6.3, sadly has no English version, lol), the transmission frequency of inter-robot communication packets have been increased to 30Hz. The API is still named `JLUI_10HzTick` because it was 10Hz for a really long time. All in all, call this function at your desired frequency and JLUI will take care of the rest.

```c
void JLUI_10HzTick();
```

## Configuration

Several macros are defined in `jlui.h` and they might be useful for you. Please read through the docs to get an overview of them.

### Total Capacity

```c
#define JLUI_TOTAL_COUNT 30
```

Each UI element takes 16 bytes in RAM. JLUI has an internal buffer which can hold *this many* UI elements. Edit at your own need.

### Sanity Check Asserts Switch

```c
#define JLUI_ENABLE_ASSERT
```

Comment out this macro to make all the asserts no-ops. Asserts are very useful when debugging, however in some cases you might have to compile your project under Debug mode without optimizations. Disabling asserts reduces CPU time wasted.

# Usage

First, call `JLUI_SetSenderReceiverId` to initialize. Do not call any other APIs before this call!

Then setup your mutex.

Then spin up a task that calls `JLUI_10HzTick` periodically.

You can now create and delete UI objects with `JLUI_Create*` and `JLUI_Delete` functions. Creation functions will return an object handle in `Uiid` type, and you can modify the object properties with the handles.

Please look at `jlui.h` to get a comprehensive view of available APIs.

# Internals

JLUI has a pool of all available object slots, whose size is determined by an aforementioned macro. When you're creating an object the library finds a slot that hasn't yet been occupied, and stuffs your graphical object there.

Each slot has a flags field, which describes what the library should do with each object when the next tick comes. For example a newly created object sets `dirtyVisibility` bit so the tick function can use the proper command to make it displayed on your player client. Any modification (color, line width, coordinates, etc) will set `dirty` bit so they will be updated at next tick.

The library uses a round-robin strategy for updating the objects to the player client. At each tick it automatically starts scanning from the beginning of the list, and stops until it has reached the limit of 7 objects in a packet, or all of the dirty objects has been processed. Each object that the library processed in a tick will be copied to a separate transmission buffer (whose size is enough to hold the largest UI packet) and the `JLUI_SendData` function will be called. After the function returns, the tick is over.

If the library has reached the 7 object limit, it will start at the offset it has stopped at last tick, and do a scan with the same strategy again.

If a dirty string object exists, it complicates the situation. Since string objects will have to occupy an entire packet, the library will transmit regular objects packet and string packet in turns to ensure the real time refresh of other critical objects, in a manner like "one string, one regular, one string, one regular, ... until no dirty string object is found". String object scanning is also in a similar "looping" fashion. Each tick that the library decides it will send a string object, it scans the entire pool to find a single string object and finishes, and next time it will start again from the place it has left.

# Optimization

- Do not modify string objects frequently. Use other objects to mask them out. This is a **VERY VERY VERY** effective trick!
- Try reducing total object count that must change between ticks down to 7 or less.

# Unfinished Functionalities

- Clear all has a special packet, which is not implemented in JLUI. When trying to delete all objects it'll actually delete the objects one by one.
- Sometimes the link may appear unstable and object creation packets might have not reached the server, and some objects might disappear for this reason. A `JLUI_RecreateAllObjects` function that sends creation packets for every object is a solution, but this is not worked on.
