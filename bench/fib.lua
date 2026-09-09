-- Recursive fibonacci, for comparison against bench/fib.c.
--
-- Same algorithm and therefore the same call count, so ns/call lines up
-- directly with what the shibe bench reports.
--
--   lua bench/fib.lua [n] [repeat]

local FIB_MAX_N = 47

local function fib(n)
	if n < 2 then return n end
	return fib(n - 1) + fib(n - 2)
end

local function fib_ref(n)
	local a, b = 0, 1
	for _ = 1, n do
		a, b = b, a + b
	end
	return a
end

-- calls(0) = calls(1) = 1, calls(n) = 1 + calls(n - 1) + calls(n - 2)
local function fib_calls(n)
	if n <= 1 then return 1 end
	local prev, cur = 1, 1
	for _ = 2, n do
		prev, cur = cur, 1 + cur + prev
	end
	return cur
end

local function usage(file)
	file:write("usage: lua bench/fib.lua [n] [repeat]\n\n")
	file:write(("  n       fibonacci index, 0 to %d (default 30)\n"):format(FIB_MAX_N))
	file:write("  repeat  timed runs to take the best of (default 3)\n")
end

local function main(argv)
	local n = 30
	local repeat_count = 3

	for i = 1, #argv do
		local arg = argv[i]
		if arg == "-h" or arg == "--help" then
			usage(io.stdout)
			return 0
		elseif i == 1 then
			n = tonumber(arg)
		elseif i == 2 then
			repeat_count = tonumber(arg)
		else
			io.stderr:write("too many arguments\n")
			usage(io.stderr)
			return 1
		end
	end

	if n == nil or n < 0 or n > FIB_MAX_N then
		io.stderr:write(("n must be between 0 and %d\n"):format(FIB_MAX_N))
		return 1
	end
	if repeat_count == nil or repeat_count < 1 then
		io.stderr:write("repeat must be at least 1\n")
		return 1
	end

	local expected = fib_ref(n)
	local calls = fib_calls(n)
	local best

	for i = 1, repeat_count do
		local start = os.clock()
		local result = fib(n)
		local elapsed = os.clock() - start

		if result ~= expected then
			io.stderr:write(("run %d returned %d, expected %d\n"):format(i, result, expected))
			return 1
		end

		if best == nil or elapsed < best then best = elapsed end
	end

	print(("lua      fib(%d) = %d"):format(n, expected))
	print(("  runs       %d"):format(repeat_count))
	print(("  best       %.4f s"):format(best))
	print(("  calls      %d"):format(calls))
	if best > 0 then
		print(("  ns/call    %.2f"):format(best * 1e9 / calls))
		print(("  calls/s    %.2f M"):format(calls / best / 1e6))
	end

	return 0
end

os.exit(main(arg))
