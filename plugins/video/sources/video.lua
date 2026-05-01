-- video - waywallen source plugin for local video wallpapers.
--
-- Installed to <prefix>/share/waywallen/sources/video.lua. Emits entries
-- with wp_type = "video" that the daemon routes to waywallen-video-renderer.
-- Metadata `video` / `path` both carry the absolute file path.

local M = {}

function M.info()
    return {
        name = "video",
        types = {"video"},
        version = "0.1.0",
    }
end

local VIDEO_EXTS = {
    mp4 = true, m4v = true, mkv = true, webm = true,
    mov = true, avi = true, flv = true, wmv = true,
    mpg = true, mpeg = true, ts = true, m2ts = true,
    ogv = true, ogm = true,
}

local function strip_ext(name)
    return name:match("(.+)%.[^.]+$") or name
end

local function first_existing(ctx, candidates)
    local out, seen = {}, {}
    for _, p in ipairs(candidates) do
        if p and p ~= "" and not seen[p] and ctx.file_exists(p) then
            seen[p] = true
            table.insert(out, p)
        end
    end
    return out
end

function M.auto_detect(ctx)
    -- Probe XDG/home defaults; return paths that actually exist so
    -- the daemon can register them as libraries.
    local home = ctx.env("HOME")
    local videos = ctx.env("XDG_VIDEOS_DIR")
    local pictures = ctx.env("XDG_PICTURES_DIR")

    local candidates = {}
    if videos and videos ~= "" then table.insert(candidates, videos) end
    if home and home ~= "" then table.insert(candidates, home .. "/Videos/Wallpapers") end
    if home and home ~= "" then table.insert(candidates, home .. "/Videos") end
    if pictures and pictures ~= "" then table.insert(candidates, pictures .. "/Wallpapers") end
    if home and home ~= "" then table.insert(candidates, home .. "/Pictures/Wallpapers") end
    return first_existing(ctx, candidates)
end

function M.scan(ctx)
    local entries = {}
    local dirs = {}
    for _, d in ipairs(ctx.libraries()) do
        if ctx.file_exists(d) then table.insert(dirs, d) end
    end
    if #dirs == 0 then
        ctx.log("video: no video libraries configured")
        return entries
    end

    local seen_path = {}
    for _, dir in ipairs(dirs) do
        -- The daemon's glob is the Rust `glob` crate, which does not expand
        -- braces. Enumerate a few depth levels explicitly — enough for the
        -- common "~/Videos/<album>/<file>" layouts without walking huge
        -- trees on every scan.
        local patterns = {
            dir .. "/*.*",
            dir .. "/*/*.*",
            dir .. "/*/*/*.*",
        }
        for _, pat in ipairs(patterns) do
            for _, path in ipairs(ctx.glob(pat)) do
                local ext = ctx.extension(path)
                if ext and VIDEO_EXTS[string.lower(ext)] and not seen_path[path] then
                    seen_path[path] = true
                    local filename = ctx.filename(path) or path
                    local name = strip_ext(filename)
                    table.insert(entries, {
                        -- Path-scoped id keeps files in different albums
                        -- with the same basename distinguishable.
                        id = "video:" .. path,
                        name = name,
                        wp_type = "video",
                        resource = path,
                        -- The QML detail panel expects an image path here;
                        -- leave video previews empty until thumbnailing exists.
                        preview = nil,
                        library_root = dir,
                        metadata = {
                            video = path,
                            path = path,
                        },
                        -- Cheap stat-only metadata. Width/height/format are
                        -- filled by the daemon's background media probe.
                        size = ctx.file_size(path),
                    })
                end
            end
        end
    end

    ctx.log("video: found " .. #entries .. " video wallpapers in "
            .. #dirs .. " directories")
    return entries
end

return M
