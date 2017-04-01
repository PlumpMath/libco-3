# libco
coroutine for windows, based on libuv 

VS2015 to build

libco depends on libuv, and few windows api

only one header except libuv

```
auto* scheduler = libco::CreateScheduler();

scheduler->NewTask([](auto* task) {
	// do some things
});

scheduler->Peek();
scheduler->Delete();
```

for now, ```ITask``` support most socket api.
