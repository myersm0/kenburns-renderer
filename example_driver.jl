using Random, JSON3

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

function send_command(cmd)
	tmp = joinpath(command_dir, "command.json.tmp")
	final = joinpath(command_dir, "command.json")
	open(tmp, "w") do f
		JSON3.write(f, cmd)
	end
	mv(tmp, final, force=true)
end

function read_status()
	path = joinpath(command_dir, "status.json")
	isfile(path) || return nothing
	try
		return JSON3.read(read(path, String))
	catch
		return nothing
	end
end

function wait_for(condition; timeout=15.0, poll_interval=0.05)
	deadline = time() + timeout
	while time() < deadline
		status = read_status()
		if status !== nothing && condition(status)
			return status
		end
		sleep(poll_interval)
	end
	return nothing
end

# launch C++ process (fire and forget)
process = run(`./slideshow $command_dir --width 5120 --height 2880`, wait=false)

sleep(0.5)

# load first image
global index = 1
kf = random_keyframe()
send_command((; command="load", path=paths[index], kf...))

wait_for(s -> s.phase == "holding")

while process_running(process)
	# queue next image
	global index = mod1(index + 1, length(paths))
	global kf = random_keyframe()
	send_command((; command="load", path=paths[index], kf...))

	# wait for preload
	wait_for(s -> s.preload_ready == true)

	# hold for a few seconds
	sleep(4.0)

	# check if still alive
	process_running(process) || break

	# trigger transition
	send_command((; command="transition"))

	# wait for transition to finish
	result = wait_for(s -> s.phase == "holding"; timeout=30.0)
	result === nothing && break
end

try wait(process) catch end
run(ignorestatus(`stty sane`))
