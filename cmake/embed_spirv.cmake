# Turns kernel_vk_{w,n}{0,1,2}.spv in DIR into the C arrays the Vulkan backend includes, in
# the form `xxd -i` gives them (which is what the Makefile uses):
#   cmake -DDIR=<directory> -DOUT=<file> -P embed_spirv.cmake
set(text "")
foreach(mode w0 w1 w2 n0 n1 n2)
  file(READ "${DIR}/kernel_vk_${mode}.spv" hex HEX)
  string(LENGTH "${hex}" digits)
  math(EXPR bytes "${digits} / 2")
  string(REGEX REPLACE "(................................)" "\\1\n" hex "${hex}")
  string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
  string(APPEND text "unsigned char kernel_vk_${mode}_spv[] = {\n${hex}\n};\n"
                     "unsigned int kernel_vk_${mode}_spv_len = ${bytes};\n")
endforeach()
file(WRITE "${OUT}" "${text}")
