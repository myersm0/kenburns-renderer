using Random

dir = ARGS[1]
command_dir = "/tmp/slideshow"
mkpath(command_dir)

paths = sort(filter(f -> endswith(lowercase(f), ".jpg"), readdir(dir, join=true)))
isempty(paths) && error("no jpg files found in $dir")

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

function wait_for(desc, condition; timeout=15.0)
	println("waiting for: $desc")
	deadline = time() + timeout
	while time() < deadline
		status = read_status()
		if status !== nothing && condition(status)
			println("  got it: $status")
			return status
		end
		sleep(0.05)
	end
	println("  timed out!")
	return nothing
end

process = run(`./slideshow $command_dir --width 5120 --height 2880`, wait=false)
sleep(0.5)

global index = 1
kf = random_keyframe()
send_load(paths[index], kf)
wait_for("first image holding", s -> s.phase == "holding")

while process_running(process)
	global index = mod1(index + 1, length(paths))
	global kf = random_keyframe()
	send_load(paths[index], kf)

	wait_for("preload ready", s -> s.preload_ready)
	sleep(4.0)
	process_running(process) || break

	send_command("""{"command":"transition"}""")

	# first wait to enter transitioning
	result = wait_for("transition started", s -> s.phase == "transitioning"; timeout=5.0)
	result === nothing && break

	# then wait for it to finish
	result = wait_for("transition done", s -> s.phase == "holding"; timeout=30.0)
	result === nothing && break
end

try wait(process) catch end
run(ignorestatus(`stty sane`))
