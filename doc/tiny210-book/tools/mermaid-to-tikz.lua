local prefix = os.getenv("DIAGRAM_PREFIX") or "diagram"
local index = 0

function CodeBlock(block)
  for _, class in ipairs(block.classes) do
    if class == "mermaid" then
      index = index + 1
      return pandoc.RawBlock("latex",
        "\\input{figures/" .. prefix .. "-" .. tostring(index) .. "}")
    end
  end
end
