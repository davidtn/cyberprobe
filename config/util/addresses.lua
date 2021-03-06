
-- Open geoip module if it exists.
local geoip
status, rtn, geoip = pcall(function() return require("geoip.country") end)
if status then
  geoip = rtn
end 

-- Open geoip database if it exists.
local geodb
if geoip then
  geodb = geoip.open()
-- Don't output, this breaks the tests.
--  print("Using GeoIP: " .. tostring(geodb))
end

local module = {}

-- Used to recurse up the stack getting all addresses in a particular protocol
-- class.
module.get_address = function(context, lst, cls, is_src)

  local par = context:get_parent()
  if par then
    module.get_address(par, lst, cls, is_src)
  end

  if is_src then
    tcls, addr = context:get_src_addr()
  else
    tcls, addr = context:get_dest_addr()
  end

  if tcls == cls then
    table.insert(lst, addr)
  end

end

-- Gets the stack of addresses on the src/dest side of a context.
module.get_stack = function(context, is_src)
  local par = context:get_parent()
  local addrs

  if par then
    if is_src then
      addrs = module.get_stack(par, true)
    else
      addrs = module.get_stack(par, false)
    end
  else
    addrs = {}
  end

  local cls, addr
  if is_src then
    cls, addr = context:get_src_addr()
  else
    cls, addr = context:get_dest_addr()
  end

  if not (cls == "root") then

    if addrs[cls] == nil then
      addrs[cls] = {}
    end

    table.insert(addrs[cls], addr)

    if cls == "ipv4" then
      if geodb then
	lookup = geodb:query_by_addr(addr)
	if lookup and lookup.code and not (lookup.code == "--") then
	  if addrs["geo"] == nil then
	    addrs["geo"] = {}
	  end
	  table.insert(addrs["geo"], lookup.code)
	end
      end
    end

  end

  return addrs
end

-- Used to recurse up the protocol stack and get protocol addresses as a
-- string.  context=protocol context, is_src=true to study source addresses,
-- otherwise it returns destination address stack.
module.describe_address = function(context, is_src)
  local par = context:get_parent()
  local str = ""
  if par then
    if is_src then
      str = module.describe_address(par, true)
    else
      str = module.describe_address(par, false)
    end
  end
  local cls, addr
  if is_src then
    cls, addr = context:get_src_addr()
  else
    cls, addr = context:get_dest_addr()
  end
  if not(addr == "") then
    if not(str == "") then
      str = str .. ":"
    end
    str = str .. addr
  end
  return str
end

return module

