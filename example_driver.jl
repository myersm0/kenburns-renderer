using Random

dir = ARGS[1]
command_dir = "/tmp/slideshow"
mkpath(command_dir)

paths = sort(filter(f -> endswith(lowercase(f), ".jpg"), readdir(dir, join=true)))
isempty(paths) && error("no jpg files found in $dir")

mutable struct EventReader
	path::String
	position::Int
end

function EventReader(command_dir)
	path = joinpath(command_dir, "events.log")
	return EventReader(path, 0)
end

function read_events(reader::EventReader)
	events = String[]
	isfile(reader.path) || return events
	open(reader.path) do f
		seek(f, reader.position)
		while !eof(f)
			push!(events, readline(f))
		end
		reader.position = position(f)
	end
	return events
end

function drain_events(reader)
	read_events(reader)
end

function has_event(events, prefix)
	any(e -> startswith(e, prefix), events)
end

function get_key_events(events)
	keys = Int[]
	for e in events
		if startswith(e, "key ")
			push!(keys, parse(Int, split(e)[2]))
		end
	end
	return keys
end

function random_keyframe()
	sx = 0.4 + 0.2 * rand()
	sy = 0.4 + 0.2 * rand()
	sz = 0.9 + 0.3 * rand()
	ex = clamp(sx + (rand() - 0.5) * 0.15, 0.2, 0.8)
	ey = clamp(sy + (rand() - 0.5) * 0.15, 0.2, 0.8)
	ez = clamp(sz + (rand() - 0.5) * 0.2, 0.8, 1.5)
	return (; start_x=sx, start_y=sy, start_zoom=sz,
		end_x=ex, end_y=ey, end_zoom=ez)
end

function send_command(cmd_json::String)
	tmp = joinpath(command_dir, "command.json.tmp")
	final_path = joinpath(command_dir, "command.json")
	write(tmp, cmd_json)
	mv(tmp, final_path, force=true)
	println("sent: $cmd_json")
end

function send_load(path, kf)
	send_command("""{"command":"load","path":"$path","start_x":$(kf.start_x),"start_y":$(kf.start_y),"start_zoom":$(kf.start_zoom),"end_x":$(kf.end_x),"end_y":$(kf.end_y),"end_zoom":$(kf.end_zoom)}""")
end

function read_status()
	path = joinpath(command_dir, "status.json")
	isfile(path) || return nothing
	try
		txt = read(path, String)
		phase = match(r"\"phase\":\"(\w+)\"", txt)
		preload = match(r"\"preload_ready\":(true|false)", txt)
		last_key = match(r"\"last_key\":(-?\d+)", txt)
		phase === nothing && return nothing
		return (;
			phase = phase[1],
			preload_ready = preload !== nothing && preload[1] == "true",
			last_key = last_key !== nothing ? parse(Int, last_key[1]) : -1,
		)
	catch e
		println("status read error: $e")
		return nothing
	end
end

function wait_for_event(desc, prefix, reader, process; timeout=15.0)
	println("waiting for: $desc")
	deadline = time() + timeout
	while time() < deadline
		process_running(process) || return nothing
		events = read_events(reader)
		if has_event(events, prefix)
			println("  got: $prefix")
			return events
		end
		sleep(0.05)
	end
	println("  timed out!")
	return nothing
end

function hold(seconds, reader, process)
	deadline = time() + seconds
	while time() < deadline
		process_running(process) || return (false, Int[])
		events = read_events(reader)
		keys = get_key_events(events)
		if !isempty(keys)
			return (true, keys)
		end
		sleep(0.05)
	end
	return (true, Int[])
end

process = run(`./slideshow $command_dir --width 1920 --height 1080 --timeout 300`, wait=false)
sleep(0.5)

reader = EventReader(command_dir)

global index = 1
kf = random_keyframe()
send_load(paths[index], kf)
wait_for_event("holding", "phase holding", reader, process)

while process_running(process)
	drain_events(reader)

	global index = mod1(index + 1, length(paths))
	kf = random_keyframe()
	send_load(paths[index], kf)

	wait_for_event("preload ready", "preload_ready", reader, process)

	alive, keys = hold(4.0, reader, process)
	alive || break

	if 32 in keys
		send_command("""{"command":"skip"}""")
		wait_for_event("skip done", "skipped", reader, process) === nothing && break
		continue
	end

	send_command("""{"command":"transition"}""")
	wait_for_event("transitioning", "phase transitioning", reader, process) === nothing && break

	# during transition, check for spacebar to skip
	deadline = time() + 30.0
	skipped = false
	while time() < deadline
		process_running(process) || break
		events = read_events(reader)
		if has_event(events, "phase holding")
			break
		end
		if 32 in get_key_events(events)
			send_command("""{"command":"skip"}""")
			wait_for_event("skip done", "skipped", reader, process)
			break
		end
		sleep(0.05)
	end
end

try wait(process) catch end
run(ignorestatus(`stty sane`))
